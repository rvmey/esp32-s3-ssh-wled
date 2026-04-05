/*
 * picture_frame.c
 *
 * TriggerCMD Picture Frame firmware loop for the Guition JC3248W535.
 *
 * Boot sequence:
 *   1. screen_init()
 *   2. WiFi — Improv-WiFi BLE provisioning if no stored credentials.
 *   3. Hardware provisioning — POST /api/hardware/provision with JWT stored
 *      in NVS.  If the JWT is absent, the HTTP config page is started and
 *      the user is prompted to visit http://<ip>/ to supply it.
 *   4. Socket.IO connect to wss://www.triggercmd.com
 *   5. GET /api/hardware/subscribeToDisplay?hardwareId=<computer_id>
 *   6. Event loop — waits for "display" events and renders received JPEGs.
 */

#include "picture_frame.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include <inttypes.h>

#include "wifi_manager.h"
#include "improv_wifi.h"
#include "screen_control.h"
#include "socketio.h"
#include "http_pf_config.h"
#include "jpeg_decoder.h"    /* espressif/esp_jpeg managed component */


static const char *TAG = "pf";

/* ── NVS helpers ────────────────────────────────────────────────────────── */

#define NVS_NS          "pf_cfg"
#define NVS_KEY_TOKEN   "hw_token"
#define NVS_KEY_COMPID  "computer_id"

#define HW_TOKEN_MAX_LEN   513   /* 512 payload + NUL */
#define COMPUTER_ID_MAX_LEN 33   /* 32 payload + NUL  */

static bool nvs_read_str(const char *key, char *out, size_t out_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(h, key, out, &out_sz);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

static esp_err_t nvs_write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── Persistent buffers (allocated in PSRAM at startup) ─────────────────── */

static uint8_t *s_dl_buf  = NULL;  /* compressed JPEG download  */
static uint8_t *s_px_buf  = NULL;  /* decoded RGB565 pixels     */

#define DL_BUF_BYTES   CONFIG_PICTURE_FRAME_MAX_IMAGE_BYTES
/* Decoded RGB565: assume up to 1024×1024 pixels (2 MB).
 * Larger source images will be rejected gracefully. */
#define PX_BUF_BYTES   (2 * 1024 * 1024)


/*
 * HTTPS GET with Bearer token auth, reads up to 2048 bytes of response body
 * into a malloc'd NUL-terminated buffer (caller must free).
 * Returns body length, or -1 on failure (HTTP error or network problem).
 */
static int https_get_auth(const char *url, const char *token, char **body)
{
    char bearer[560];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    *body = NULL;

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", bearer);

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return -1;
    }

    int64_t cl = esp_http_client_fetch_headers(client);
    int max_body = 2048;
    if (cl > 0 && cl < max_body) max_body = (int)cl;

    char *buf = malloc(max_body + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    int total = 0;
    while (total < max_body) {
        int n = esp_http_client_read(client, buf + total, max_body - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTPS GET %s → HTTP %d", url, status);
        free(buf);
        return -1;
    }

    *body = buf;
    return total;
}

/* ── JPEG download into PSRAM buffer ────────────────────────────────────── */

static int download_jpeg(const char *url)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "download_jpeg open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_http_client_fetch_headers(client);

    int total = 0;
    int max   = DL_BUF_BYTES;
    while (total < max) {
        int n = esp_http_client_read(client,
                                     (char *)s_dl_buf + total,
                                     max - total);
        if (n <= 0) break;
        total += n;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "download_jpeg %s → HTTP %d", url, status);
        return -1;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes from %s (HTTP %d)", total, url, status);
    return total;
}

/* ── Minimal JSON string-value extractor ────────────────────────────────── */
/*
 * Extracts a string value from a flat JSON object for the given key.
 * Handles basic backslash escapes.  Does NOT handle nested objects or
 * arrays inside the target value — which is fine for our use case.
 * Returns true and writes to out if the key is found and value is non-empty.
 */
static bool json_extract_str(const char *json, const char *key,
                              char *out, size_t out_sz)
{
    /* Build search pattern: "key" */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    /* skip whitespace and ':' */
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++; /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i > 0;
}

/* ── Socket.IO event handler ─────────────────────────────────────────────── */

static void pf_event_handler(const char *event_name,
                              const char *payload_json,
                              void       *ctx)
{
    (void)ctx;

    if (strcmp(event_name, "display") != 0) return;

    /* Extract "url" and optional "caption" from payload JSON without cJSON.
     * Payload format: {"url":"https://...","caption":"optional text"}   */
    static char s_url[512];
    static char s_caption[128];
    s_caption[0] = '\0';

    if (!json_extract_str(payload_json, "url", s_url, sizeof(s_url)) ||
            s_url[0] == '\0') {
        ESP_LOGE(TAG, "display event missing 'url'");
        return;
    }
    json_extract_str(payload_json, "caption", s_caption, sizeof(s_caption));

    const char *url = s_url;
    ESP_LOGI(TAG, "display event: url=%s", url);

    /* Download JPEG */
    int dl_bytes = download_jpeg(url);
    if (dl_bytes <= 0) {
        screen_draw_text("Image download\nfailed");
        return;
    }

    /* Decode JPEG → RGB565 (little-endian uint16_t, swap_color_bytes = 0) */
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata      = s_dl_buf,
        .indata_size = (uint32_t)dl_bytes,
        .outbuf      = s_px_buf,
        .outbuf_size = PX_BUF_BYTES,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
        .flags       = {
            .swap_color_bytes = 0,  /* keep little-endian: LSByte first */
        },
    };

    esp_jpeg_image_output_t outimg = {0};
    esp_err_t dec_ret = esp_jpeg_decode(&jpeg_cfg, &outimg);
    if (dec_ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(dec_ret));
        screen_draw_text("Image decode\nfailed");
        return;
    }

    ESP_LOGI(TAG, "JPEG decoded: %"PRIu32"x%"PRIu32, outimg.width, outimg.height);

    /* Render scaled to full panel */
    screen_draw_rgb565(s_px_buf, (int)outimg.width, (int)outimg.height);

    /* Overlay caption if present */
    if (s_caption[0]) {
        screen_draw_text(s_caption);
    }
}

/* ── Connection step: Socket.IO + subscribeToDisplay ───────────────────── */

static esp_err_t connect_and_subscribe(const char *hw_token,
                                       const char *computer_id)
{
    screen_draw_text("Connecting to server...");

    esp_err_t ret = socketio_connect(
            "wss://www.triggercmd.com/socket.io/?EIO=4&transport=websocket",
            hw_token,
            pf_event_handler,
            NULL);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "socketio_connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Socket.IO is now connected; call subscribeToDisplay */
    char sub_url[128];
    snprintf(sub_url, sizeof(sub_url),
             "https://www.triggercmd.com/api/hardware/subscribeToDisplay"
             "?hardwareId=%s", computer_id);

    char *body = NULL;
    int body_len = https_get_auth(sub_url, hw_token, &body);
    if (body_len > 0 && body) {
        char sub_msg[128] = {0};
        if (json_extract_str(body, "message", sub_msg, sizeof(sub_msg)) &&
                sub_msg[0]) {
            screen_draw_text(sub_msg);
            ESP_LOGI(TAG, "subscribeToDisplay: %s", sub_msg);
        }
        free(body);
    } else {
        ESP_LOGW(TAG, "subscribeToDisplay request failed");
        screen_draw_text("Subscribe failed\n(will retry)");
    }

    return ESP_OK;
}

/* ── Main entry point ───────────────────────────────────────────────────── */

void picture_frame_run(void)
{
    /* Initialise display first — screen_init() creates s_draw_mutex which all
     * screen_draw_*() helpers require.  Must happen before any screen call. */
    screen_init();

    /* Allocate persistent PSRAM buffers */
    s_dl_buf = heap_caps_malloc(DL_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_px_buf = heap_caps_malloc(PX_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_dl_buf || !s_px_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers (dl=%p px=%p)",
                 s_dl_buf, s_px_buf);
        screen_draw_text("PSRAM alloc\nfailed — halting");
        vTaskSuspend(NULL);
    }

    /* ── WiFi ────────────────────────────────────────────────────────────── */
    screen_draw_text("Waiting for WiFi...");

    if (!wifi_has_stored_credentials()) {
        screen_set_color(0, 0, 64);   /* blue = BLE provisioning */
        if (improv_wifi_start() != ESP_OK) {
            screen_set_color(64, 0, 0);
            ESP_LOGE(TAG, "Improv WiFi provisioning failed");
            vTaskSuspend(NULL);
        }
    }

    if (wifi_connect() != ESP_OK) {
        screen_set_color(64, 0, 0);
        ESP_LOGE(TAG, "WiFi connect failed");
        vTaskSuspend(NULL);
    }

    screen_set_color(0, 32, 0);   /* green = connected */
    vTaskDelay(pdMS_TO_TICKS(500));
    screen_off();

    /* ── NVS: read hw_token and computer_id ─────────────────────────────── */
    char hw_token[HW_TOKEN_MAX_LEN]     = {0};
    char computer_id[COMPUTER_ID_MAX_LEN] = {0};

    bool have_token    = nvs_read_str(NVS_KEY_TOKEN,  hw_token,    sizeof(hw_token));
    bool have_comp_id  = nvs_read_str(NVS_KEY_COMPID, computer_id, sizeof(computer_id));

    /* If no hardware token, serve the config page until the user adds one */
    if (!have_token) {
        ESP_LOGI(TAG, "No HW token — starting config server");
        http_pf_config_start();

        char ip_msg[80] = "0.0.0.0";
        wifi_get_ip(ip_msg, sizeof(ip_msg));
        char disp_msg[128];
        snprintf(disp_msg, sizeof(disp_msg),
                 "Visit http://%s/\nto configure", ip_msg);
        screen_draw_text(disp_msg);

        /* Poll NVS until token appears, then reboot */
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (nvs_read_str(NVS_KEY_TOKEN, hw_token, sizeof(hw_token))) {
                ESP_LOGI(TAG, "HW token stored — rebooting");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
    }

    /* ── Provisioning: get computer_id if not already stored ────────────── */
    if (!have_comp_id) {
        ESP_LOGI(TAG, "No computer_id — calling provision API");
        screen_draw_text("Provisioning...");

        char *body = NULL;
        int body_len = https_get_auth(
                "https://www.triggercmd.com/api/hardware/provision",
                hw_token, &body);

        if (body_len > 0 && body) {
            char prov_computer[COMPUTER_ID_MAX_LEN] = {0};
            char prov_msg[128] = {0};

            if (json_extract_str(body, "computer",
                                 prov_computer, sizeof(prov_computer)) &&
                    prov_computer[0]) {
                strncpy(computer_id, prov_computer, sizeof(computer_id) - 1);
                nvs_write_str(NVS_KEY_COMPID, computer_id);
                ESP_LOGI(TAG, "computer_id stored: %s", computer_id);
            }

            if (json_extract_str(body, "message",
                                 prov_msg, sizeof(prov_msg)) && prov_msg[0]) {
                screen_draw_text(prov_msg);
            }

            free(body);
        } else {
            ESP_LOGE(TAG, "Provision request failed");
            screen_draw_text("Provision failed\nRetrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* ── Connect + subscribe loop ────────────────────────────────────────── */
    while (true) {
        esp_err_t ret = connect_and_subscribe(hw_token, computer_id);
        if (ret != ESP_OK) {
            screen_draw_text("Server connect\nfailed\nRetrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            socketio_disconnect();
            continue;
        }

        /* Monitor connection and reconnect on drop */
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!socketio_connected()) {
                ESP_LOGW(TAG, "Socket.IO disconnected — reconnecting");
                screen_draw_text("Reconnecting...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                socketio_disconnect();
                break;   /* outer loop will reconnect */
            }
        }
    }
}
