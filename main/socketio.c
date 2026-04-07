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
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "triggercmd_ca.h"   /* embedded Go Daddy Root G2 cert for triggercmd.com */
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

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

    esp_websocket_client_config_t cfg = {
        .uri               = uri,
        .headers           = auth_hdr,
        .cert_pem          = TRIGGERCMD_CA_PEM,   /* GoDaddy Root G2 — avoids cross-signed bundle lookup */
        .ping_interval_sec = 25,   /* keep-alive through NAT idle timeouts */
        .buffer_size       = 4096,
    };

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
        return ret;
    }

    /* Block until "40" SIO connect ack is received, or timeout */
    EventBits_t bits = xEventGroupWaitBits(s_evt_group,
                                           SIO_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(SIO_CONNECT_TIMEOUT_MS));
    if (!(bits & SIO_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "SIO connect timeout (%d ms)", SIO_CONNECT_TIMEOUT_MS);
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
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
