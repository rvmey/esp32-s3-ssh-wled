/*
 * socketio.c
 *
 * Minimal Engine.IO v4 + Socket.IO v4 client.
 * Implemented directly over espressif/esp_websocket_client so no additional
 * Socket.IO library is required.
 *
 * Protocol framing handled:
 *   Inbound "0{…}"  – EIO open  → send "40" (SIO connect)
 *   Inbound "2"     – EIO ping  → reply "3" (EIO pong)
 *   Inbound "40{…}" – SIO connect ack → signal socketio_connect() caller
 *   Inbound "42[…]" – SIO event → invoke user callback
 *   Inbound "41"    – SIO disconnect → clear connected flag
 */

#include "socketio.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "triggercmd_ca.h"   /* embedded Go Daddy Root G2 cert for triggercmd.com */
#include "mbedtls/error.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"

static const char *TAG = "socketio";

#define SIO_CONNECTED_BIT       BIT0
#define SIO_CONNECT_TIMEOUT_MS  10000

/* ── State ──────────────────────────────────────────────────────────────── */

static esp_websocket_client_handle_t s_client    = NULL;
static socketio_event_cb_t           s_cb        = NULL;
static void                         *s_user_ctx  = NULL;
static volatile bool                 s_connected = false;
static EventGroupHandle_t            s_evt_group = NULL;

/* ── WebSocket event handler ─────────────────────────────────────────────── */

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch ((esp_websocket_event_id_t)event_id) {

    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS transport connected");
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (!d || d->data_len <= 0 || !d->data_ptr) break;

        const char *p = d->data_ptr;
        int         n = d->data_len;

        /* Log every raw frame so we can see exactly what arrives */
        {
            char dbg[257];
            int  dl = n < 256 ? n : 256;
            memcpy(dbg, p, dl);
            dbg[dl] = '\0';
            ESP_LOGI(TAG, "WS RX (%d bytes): %s", n, dbg);
        }

        /* EIO open: "0{…}" */
        if (p[0] == '0') {
            ESP_LOGI(TAG, "EIO open received; sending SIO connect (40)");
            esp_websocket_client_send_text(s_client, "40", 2, pdMS_TO_TICKS(3000));
            break;
        }

        /* EIO ping: "2" */
        if (n == 1 && p[0] == '2') {
            esp_websocket_client_send_text(s_client, "3", 1, pdMS_TO_TICKS(3000));
            break;
        }

        /* Need at least 2 bytes for SIO framing */
        if (n < 2) break;

        /* SIO connect ack: "40{…}" */
        if (p[0] == '4' && p[1] == '0') {
            s_connected = true;
            if (s_evt_group) {
                xEventGroupSetBits(s_evt_group, SIO_CONNECTED_BIT);
            }
            ESP_LOGI(TAG, "SIO connect ack received — connected");
            break;
        }

        /* SIO event: "42[\"event_name\", payload]" */
        if (p[0] == '4' && p[1] == '2') {
            /* Minimal parse of ["name", payload] without cJSON.
             * Format: 42["event_name", {payload}]            */
            const char *arr_start = p + 2;
            int         arr_n    = n - 2;

            /* Find opening quote of event name */
            const char *q = memchr(arr_start, '"', arr_n);
            if (!q) break;
            q++; /* skip opening quote */
            int remaining = arr_n - (int)(q - arr_start);
            const char *qend = memchr(q, '"', remaining);
            if (!qend) break;

            int name_len = (int)(qend - q);
            char event_name[64];
            if (name_len >= (int)sizeof(event_name)) name_len = (int)sizeof(event_name) - 1;
            memcpy(event_name, q, name_len);
            event_name[name_len] = '\0';

            /* Payload starts after ',' past the closing name quote */
            const char *payload_start = qend + 1;
            int payload_remaining = arr_n - (int)(payload_start - arr_start);
            while (payload_remaining > 0 &&
                   (*payload_start == ',' || *payload_start == ' ')) {
                payload_start++;
                payload_remaining--;
            }
            /* Trim trailing ']' */
            while (payload_remaining > 0 &&
                   payload_start[payload_remaining - 1] == ']') {
                payload_remaining--;
            }

            ESP_LOGI(TAG, "SIO event name='%s' payload=%.*s",
                     event_name,
                     payload_remaining > 200 ? 200 : payload_remaining,
                     payload_start);

            if (payload_remaining > 0 && s_cb) {
                char *payload_str = strndup(payload_start, payload_remaining);
                if (payload_str) {
                    s_cb(event_name, payload_str, s_user_ctx);
                    free(payload_str);
                }
            }
            break;
        }

        /* SIO ack reply to virtual GET: "43[...]" — log full content so we can
         * see whether subscribeToFunRoom returned 200 or an error status. */
        if (n >= 2 && p[0] == '4' && p[1] == '3') {
            /* Log up to 400 bytes of the ack body */
            int log_len = n < 400 ? n : 400;
            ESP_LOGI(TAG, "SIO ack (vget response, %d bytes): %.*s", n, log_len, p);
            break;
        }

        /* SIO disconnect: "41" */
        if (p[0] == '4' && p[1] == '1') {
            s_connected = false;
            ESP_LOGI(TAG, "SIO disconnect received");
            break;
        }

        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WS disconnected");
        s_connected = false;
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WS closed by server");
        s_connected = false;
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS error");
        /* TEMP DIAG: dump full TLS error details */
        if (d) {
            ESP_LOGE(TAG, "WS error detail: error_type=%d tls_last_err=0x%x tls_stack_err=0x%x verify_flags=0x%x sock_errno=%d",
                     (int)d->error_handle.error_type,
                     (unsigned)d->error_handle.esp_tls_last_esp_err,
                     (unsigned)d->error_handle.esp_tls_stack_err,
                     (unsigned)d->error_handle.esp_tls_cert_verify_flags,
                     d->error_handle.esp_transport_sock_errno);

            if (d->error_handle.esp_tls_cert_verify_flags) {
                int flags = d->error_handle.esp_tls_cert_verify_flags;
                char verify_info[512];
                mbedtls_x509_crt_verify_info(verify_info, sizeof(verify_info), "  ! ", flags);
                ESP_LOGE(TAG, "verify info:\n%s", verify_info);
                if (flags & MBEDTLS_X509_BADCERT_EXPIRED) ESP_LOGE(TAG, "verify flag: MBEDTLS_X509_BADCERT_EXPIRED");
                if (flags & MBEDTLS_X509_BADCERT_REVOKED) ESP_LOGE(TAG, "verify flag: MBEDTLS_X509_BADCERT_REVOKED");
                if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) ESP_LOGE(TAG, "verify flag: MBEDTLS_X509_BADCERT_CN_MISMATCH");
                if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) ESP_LOGE(TAG, "verify flag: MBEDTLS_X509_BADCERT_NOT_TRUSTED");
                if (flags & MBEDTLS_X509_BADCERT_FUTURE) ESP_LOGE(TAG, "verify flag: MBEDTLS_X509_BADCERT_FUTURE");
                if (flags & MBEDTLS_X509_BADCRL_NOT_TRUSTED) ESP_LOGE(TAG, "verify flag: MBEDTLS_X509_BADCRL_NOT_TRUSTED");
            }

            if (d->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT &&
                d->error_handle.esp_tls_stack_err != 0) {
                char mbedtls_err_buf[128];
                int mbedtls_err = d->error_handle.esp_tls_stack_err;
                if (mbedtls_err > 0) {
                    mbedtls_err = -mbedtls_err;
                }
                mbedtls_strerror(mbedtls_err,
                                 mbedtls_err_buf, sizeof(mbedtls_err_buf));
                ESP_LOGE(TAG, "mbedtls error string: %s", mbedtls_err_buf);
            }
        }
        s_connected = false;
        break;

    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t socketio_connect(const char          *uri,
                           const char          *auth_token,
                           socketio_event_cb_t  cb,
                           void                *user_ctx)
{
    s_cb        = cb;
    s_user_ctx  = user_ctx;
    s_connected = false;

    if (!s_evt_group) {
        s_evt_group = xEventGroupCreate();
        if (!s_evt_group) return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_evt_group, SIO_CONNECTED_BIT);

    /* Build the Authorization header string.
     * auth_token is guaranteed ≤ 511 chars by the NVS layout.
     * "Authorization: Bearer " = 22 chars; "\r\n" = 2; total ≤ 535 → fits in 600. */
    char auth_hdr[600];
    snprintf(auth_hdr, sizeof(auth_hdr),
             "Authorization: Bearer %s\r\n", auth_token);

    /* DIAG: log device time to detect epoch/1970 clock causing cert-date failure */
    time_t now = time(NULL);
    ESP_LOGW(TAG, "DIAG device time: %lld (0 or small = not synced, cert may fail date check)",
             (long long)now);

    /* Parse URI once and feed explicit host/path/port into websocket config.
     * This avoids any ambiguity in URI parsing affecting SNI/Host handling. */
    bool secure_transport = (strncmp(uri, "wss://", 6) == 0);
    const char *host_start = strstr(uri, "://");
    host_start = host_start ? (host_start + 3) : uri;
    const char *path_start = strchr(host_start, '/');
    const char *host_end = path_start ? path_start : (host_start + strlen(host_start));

    char ws_host[96];
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(ws_host)) {
        ESP_LOGE(TAG, "Invalid websocket host in uri: %s", uri);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(ws_host, host_start, host_len);
    ws_host[host_len] = '\0';

    int ws_port = secure_transport ? 443 : 80;
    char *colon = strchr(ws_host, ':');
    if (colon) {
        int parsed_port = atoi(colon + 1);
        if (parsed_port > 0 && parsed_port <= 65535) {
            ws_port = parsed_port;
        }
        *colon = '\0';
    }

    char ws_path[192];
    if (path_start && *path_start) {
        strlcpy(ws_path, path_start, sizeof(ws_path));
    } else {
        strlcpy(ws_path, "/", sizeof(ws_path));
    }

    ESP_LOGI(TAG, "WS target parsed: host=%s port=%d secure=%d path=%s",
             ws_host, ws_port, secure_transport ? 1 : 0, ws_path);

    for (int attempt = 0; attempt < 3; ++attempt) {
        bool use_bundle = (attempt == 2);
        bool use_root_only = (attempt == 1);

        esp_websocket_client_config_t cfg = {
            .uri                 = uri,
            .host                = ws_host,
            .port                = ws_port,
            .path                = ws_path,
            .transport           = secure_transport ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_OVER_TCP,
            .headers             = auth_hdr,
            .network_timeout_ms  = 20000,
            .reconnect_timeout_ms = 5000,
            .disable_auto_reconnect = true,
            .ping_interval_sec   = 25,   /* keep-alive through NAT idle timeouts */
            .buffer_size         = 4096,
            .cert_common_name    = "www.triggercmd.com",
            .skip_cert_common_name_check = false,
        };

        if (use_bundle) {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            cfg.cert_common_name = "www.triggercmd.com";
            cfg.skip_cert_common_name_check = false;
            ESP_LOGW(TAG, "WS TLS attempt %d/3 using ESP cert bundle", attempt + 1);
        } else if (use_root_only) {
            cfg.cert_pem = TRIGGERCMD_ROOT_G2_PEM;
            ESP_LOGW(TAG, "WS TLS attempt %d/3 using TriggerCMD root-only cert", attempt + 1);
        } else {
            cfg.cert_pem = TRIGGERCMD_CA_PEM;
            /* Leave cert_len at 0 for PEM so esp_websocket_client does not treat it as DER. */
            ESP_LOGI(TAG, "WS TLS attempt %d/3 using TriggerCMD embedded cert", attempt + 1);
        }

        ESP_LOGI(TAG, "WS TLS attempt %d/3 heap: free=%u min=%u",
                 attempt + 1,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
        ESP_LOGI(TAG, "WS TLS attempt %d/3 internal heap: free=%u largest=%u",
             attempt + 1,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

        s_client = esp_websocket_client_init(&cfg);
        if (!s_client) {
            ESP_LOGE(TAG, "esp_websocket_client_init failed");
            return ESP_FAIL;
        }

        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                      ws_event_handler, NULL);

        esp_err_t ret = esp_websocket_client_start(s_client);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_websocket_client_start: %s", esp_err_to_name(ret));
            esp_websocket_client_destroy(s_client);
            s_client = NULL;
            if (attempt == 2) return ret;
            continue;
        }

        /* Block until "40" SIO connect ack is received, or timeout */
        EventBits_t bits = xEventGroupWaitBits(s_evt_group,
                                               SIO_CONNECTED_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(SIO_CONNECT_TIMEOUT_MS));
        if (bits & SIO_CONNECTED_BIT) {
            return ESP_OK;
        }

        ESP_LOGE(TAG, "SIO connect timeout (%d ms) on attempt %d/3",
             SIO_CONNECT_TIMEOUT_MS, attempt + 1);
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }

    return ESP_ERR_TIMEOUT;
}

void socketio_disconnect(void)
{
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
}

bool socketio_connected(void)
{
    return s_connected;
}

esp_err_t socketio_send_vget(const char *path, const char *auth_token)
{
    if (!s_client || !s_connected) return ESP_ERR_INVALID_STATE;

    /* Match the Python sails_websocket.py wire format exactly — no ack ID,
     * no _isSailsSocketRequest (not recognised by sails-hook-sockets < 1.x).
     * The __sails_io_sdk_version is already appended to the path by the caller
     * AND to the WebSocket handshake URL, which is what Sails 0.12.x checks
     * in parseVirtualRequest to identify a valid sails.io SDK client. */
    char msg[768];
    snprintf(msg, sizeof(msg),
             "421[\"get\",{\"url\":\"%s\","
             "\"headers\":{\"Authorization\":\"Bearer %s\"},"
             "\"data\":{}}]",
             path, auth_token);

    ESP_LOGI(TAG, "Sending vget: %.150s", msg);

    int len = (int)strlen(msg);
    int sent = esp_websocket_client_send_text(s_client, msg, len, pdMS_TO_TICKS(3000));
    return (sent >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t socketio_send_vpost(const char *path,
                              const char *auth_token,
                              const char *data_json_object)
{
    if (!s_client || !s_connected) return ESP_ERR_INVALID_STATE;
    if (!path || !auth_token || !data_json_object) return ESP_ERR_INVALID_ARG;

    char msg[1024];
    int n = snprintf(msg, sizeof(msg),
                     "421[\"post\",{\"url\":\"%s\","
                     "\"headers\":{\"Authorization\":\"Bearer %s\"},"
                     "\"data\":%s}]",
                     path, auth_token, data_json_object);
    if (n <= 0 || n >= (int)sizeof(msg)) {
        ESP_LOGE(TAG, "vpost message too large");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Sending vpost: %.200s", msg);
    int sent = esp_websocket_client_send_text(s_client, msg, n, pdMS_TO_TICKS(3000));
    return (sent >= 0) ? ESP_OK : ESP_FAIL;
}

void socketio_send_eio_ping(void)
{
    if (!s_client || !s_connected) return;
    /* EIO v2/v3 client-initiated keepalive: client sends "2", server replies "3".
     * Sails.js 0.12 / socket.io 1.x will close the socket after pingTimeout (60 s)
     * if it receives no ping from the client. */
    esp_websocket_client_send_text(s_client, "2", 1, pdMS_TO_TICKS(3000));
    ESP_LOGD(TAG, "EIO ping sent");
}
