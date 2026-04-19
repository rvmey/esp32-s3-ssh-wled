/*
 * tcmd_atom_echo.c — TRIGGERcmd ATOM Echo firmware variant.
 *
 * Boot flow:
 *   1. Init LED (yellow) + audio + mic
 *   2. WiFi: if no stored credentials → SoftAP provisioning page
 *   3. wifi_connect()
 *   4. If no stt_key in NVS → SoftAP config page to collect it
 *   5. Pair code loop: GET /pair/index → speak code → poll /pair/lookup
 *   6. Idle: short-press = health check, long-press = voice query
 *
 * WiFi provisioning and STT-key collection both use the same pattern:
 *   - ESP32 starts a SoftAP ("TCMD-Echo-XXXXXX") + HTTP server on port 80
 *   - User connects and fills in a form
 *   - Credentials saved to NVS, device restarts
 *
 * NVS namespace: "ae_cfg"
 *   hw_token       — TRIGGERcmd user JWT
 *   conversation_id — persisted Chat API conversation ID
 *   stt_key        — OpenAI API key for Whisper STT
 */

#include "tcmd_atom_echo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "wifi_manager.h"
#include "atom_led.h"
#include "atom_audio.h"
#include "atom_mic.h"
#include "audio_clips.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

#define TAG             "atom_echo"

#define BUTTON_GPIO     39       /* active-low */
#define LONG_PRESS_MS   1000

#define NVS_NS              "ae_cfg"
#define NVS_KEY_TOKEN       "hw_token"
#define NVS_KEY_CONV        "conversation_id"
#define NVS_KEY_STT         "stt_key"

#define TCMD_BASE_URL       "https://www.triggercmd.com"  /* prod server */
// #define TCMD_BASE_URL       "http://192.168.86.248:1337"  /* local dev server */
#define PAIR_ENDPOINT       TCMD_BASE_URL "/pair/index"
#define PAIR_LOOKUP_URL     TCMD_BASE_URL "/pair/lookup"
#define HEALTH_URL          TCMD_BASE_URL "/api/v1/chat/conversations"
#define CHAT_MSG_URL        TCMD_BASE_URL "/api/v1/chat/message"

/* Whisper STT endpoint — requires an OpenAI API key in NVS */
#define STT_URL             "https://api.openai.com/v1/audio/transcriptions"

#define PAIR_POLL_INTERVAL_MS   5000
#define PAIR_MAX_POLLS          120    /* 10 minutes */
#define PAIR_RESPEECH_INTERVAL  6      /* re-speak every 30 s (6 × 5 s polls) */

#define HTTP_TIMEOUT_MS     20000
#define MAX_BODY            2048
#define HW_TOKEN_MAX        512
#define CONV_ID_MAX         64
#define STT_KEY_MAX         256
#define TRANSCRIPT_MAX      512

/* ── SoftAP provisioning ─────────────────────────────────────────────────── */

/* HTML page served during SoftAP provisioning for WiFi credentials */
static const char s_wifi_prov_html[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TCMD Echo Setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh}"
    ".card{background:#1e2130;border:1px solid #2d3148;border-radius:1rem;"
    "padding:2rem;max-width:380px;width:100%}"
    "h1{color:#a5b4fc;font-size:1.3rem;margin:0 0 1rem}"
    "label{display:block;color:#94a3b8;font-size:.85rem;margin:.75rem 0 .25rem}"
    "input{width:100%;box-sizing:border-box;padding:.6rem;background:#0f1117;"
    "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:1rem}"
    "button{margin-top:1.25rem;width:100%;padding:.75rem;background:#4f46e5;"
    "color:#fff;border:none;border-radius:.6rem;font-size:1rem;cursor:pointer}"
    "</style></head><body><div class='card'>"
    "<h1>TRIGGERcmd Echo Setup</h1>"
    "<form method='POST' action='/save'>"
    "<label>Wi-Fi SSID</label><input name='ssid' required>"
    "<label>Wi-Fi Password</label><input name='pass' type='password'>"
    "<label>OpenAI API Key (for voice)</label><input name='stt' placeholder='sk-...'>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form></div></body></html>";

static const char s_saved_html[] =
    "<!DOCTYPE html><html><body style='font-family:system-ui;background:#0f1117;"
    "color:#86efac;display:flex;align-items:center;justify-content:center;"
    "min-height:100vh'><h2>Saved! Device is restarting...</h2></body></html>";

/* ── NVS helpers ─────────────────────────────────────────────────────────── */

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

/* ── URL decode helper ───────────────────────────────────────────────────── */

static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;
    while (*src && i < dst_sz - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* Extract a field value from application/x-www-form-urlencoded body */
static bool form_get_field(const char *body, const char *field,
                            char *out, size_t out_sz)
{
    size_t flen = strlen(field);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, field, flen) == 0 && p[flen] == '=') {
            p += flen + 1;
            const char *end = strchr(p, '&');
            char encoded[256] = {0};
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            if (vlen >= sizeof(encoded)) vlen = sizeof(encoded) - 1;
            memcpy(encoded, p, vlen);
            url_decode(encoded, out, out_sz);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* ── SoftAP provisioning server ──────────────────────────────────────────── */

static bool s_prov_done = false;

static esp_err_t prov_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_wifi_prov_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t prov_save_handler(httpd_req_t *req)
{
    char body[1024] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[len] = '\0';

    char ssid[64]  = {0};
    char pass[64]  = {0};
    char stt[STT_KEY_MAX]  = {0};

    form_get_field(body, "ssid", ssid, sizeof(ssid));
    form_get_field(body, "pass", pass, sizeof(pass));
    form_get_field(body, "stt",  stt,  sizeof(stt));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_saved_html, HTTPD_RESP_USE_STRLEN);

    if (ssid[0]) {
        wifi_save_credentials(ssid, pass);
        ESP_LOGI(TAG, "WiFi credentials saved for SSID: %s", ssid);
    }
    if (stt[0]) {
        nvs_write_str(NVS_KEY_STT, stt);
        ESP_LOGI(TAG, "STT API key saved");
    }

    s_prov_done = true;
    return ESP_OK;
}

/* Start SoftAP + HTTP server; block until the form is submitted */
static void run_softap_provisioning(void)
{
    /* Ensure WiFi stack is initialised before using any WiFi API */
    wifi_stack_init_public();

    /* Build AP name from last 3 bytes of MAC */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "TCMD-Echo-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Starting SoftAP: %s", ap_ssid);

    /* Init WiFi in AP mode (may already be init'd by wifi_manager) */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    (void)ap_netif;

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Start HTTP server */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_open_sockets = 4;  /* provisioning: AP-only, no STA sockets in use */
    http_cfg.lru_purge_enable = true;
    httpd_handle_t server   = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    httpd_uri_t get_uri  = { .uri = "/",     .method = HTTP_GET,  .handler = prov_get_handler  };
    httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = prov_save_handler };
    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &save_uri);

    /* Blue blink while waiting for provisioning */
    bool led_on = false;
    s_prov_done = false;

    ESP_LOGI(TAG, "Connect to '%s' and browse to 192.168.4.1", ap_ssid);

    while (!s_prov_done) {
        led_on = !led_on;
        if (led_on) atom_led_set(0, 0, 48);
        else        atom_led_off();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    httpd_stop(server);
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* Brief pause, then restart to pick up saved credentials cleanly */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── JSON helpers ────────────────────────────────────────────────────────── */

static bool json_extract_str(const char *json, const char *key,
                              char *out, size_t out_sz)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
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

/* ── HTTPS helpers ───────────────────────────────────────────────────────── */

/* HTTPS GET with no auth.  Returns body length or -1 on error. */
static int https_get_simple(const char *url, char **body_out)
{
    *body_out = NULL;
    esp_http_client_config_t cfg = {
        .url             = url,
        .method          = HTTP_METHOD_GET,
        .timeout_ms      = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    int64_t cl       = esp_http_client_fetch_headers(client);
    int     max_body = MAX_BODY;
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
        ESP_LOGE(TAG, "GET %s → HTTP %d", url, status);
        free(buf);
        return -1;
    }
    *body_out = buf;
    return total;
}

/* HTTPS GET with Bearer token auth.  Returns body length or -1 on error. */
static int https_get_auth(const char *url, const char *token, char **body_out)
{
    *body_out = NULL;
    char bearer[HW_TOKEN_MAX + 10];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", bearer);

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    int64_t cl       = esp_http_client_fetch_headers(client);
    int     max_body = MAX_BODY;
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
        ESP_LOGE(TAG, "GET (auth) %s → HTTP %d", url, status);
        free(buf);
        return -1;
    }
    *body_out = buf;
    return total;
}

/* HTTPS POST with JSON body and Bearer token auth.
 * Returns HTTP status code or -1 on network error. */
static int https_post_json(const char *url, const char *token,
                           const char *json_body, char **resp_out)
{
    if (resp_out) *resp_out = NULL;
    char bearer[HW_TOKEN_MAX + 10];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    int body_len = (int)strlen(json_body);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    if (esp_http_client_open(client, body_len) != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_write(client, json_body, body_len);

    int64_t cl       = esp_http_client_fetch_headers(client);
    int     max_body = MAX_BODY;
    if (cl > 0 && cl < max_body) max_body = (int)cl;

    if (resp_out) {
        char *buf = malloc(max_body + 1);
        if (buf) {
            int total = 0;
            while (total < max_body) {
                int n = esp_http_client_read(client, buf + total, max_body - total);
                if (n <= 0) break;
                total += n;
            }
            buf[total] = '\0';
            *resp_out = buf;
        }
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

/* ── Pair code speech ────────────────────────────────────────────────────── */

static void speak_pair_code(const char *code)
{
    for (int i = 0; code[i]; i++) {
        audio_clip_t clip = clip_for_char(code[i]);
        if (clip.samples && clip.num_samples > 0) {
            atom_audio_play_clip(clip.samples, clip.num_samples,
                                 AUDIO_CLIP_SAMPLE_RATE);
        } else {
            /* Stub clip: play a short silence gap so timing is consistent */
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

/* ── Button press detection ──────────────────────────────────────────────── */

typedef enum { BTN_NONE, BTN_SHORT, BTN_LONG } btn_press_t;

/* Non-blocking poll: checks current button state.
 * BTN_SHORT — returned on release after a short press.
 * BTN_LONG  — returned while the button is STILL HELD (once the long-press
 *             threshold is crossed).  This lets the caller (do_voice_query)
 *             start recording immediately and use the physical button release
 *             to stop it. */
static btn_press_t poll_button(void)
{
    static bool     s_pressed     = false;
    static uint32_t s_press_start = 0;
    static bool     s_long_fired  = false;

    bool currently_pressed = (gpio_get_level(BUTTON_GPIO) == 0);

    if (currently_pressed && !s_pressed) {
        /* Falling edge — button just pressed */
        s_pressed     = true;
        s_long_fired  = false;
        s_press_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    } else if (currently_pressed && s_pressed && !s_long_fired) {
        /* Still held — fire BTN_LONG as soon as threshold is crossed */
        uint32_t held_ms = xTaskGetTickCount() * portTICK_PERIOD_MS - s_press_start;
        if (held_ms >= LONG_PRESS_MS) {
            s_long_fired = true;
            return BTN_LONG;   /* button is still down — mic loop will wait for release */
        }
    } else if (!currently_pressed && s_pressed) {
        /* Rising edge — button released */
        s_pressed = false;
        if (!s_long_fired) {
            return BTN_SHORT;  /* released before long-press threshold */
        }
    }
    return BTN_NONE;
}

/* ── STT: POST WAV to Whisper API ────────────────────────────────────────── */

/*
 * Posts a WAV buffer to the OpenAI Whisper API using multipart/form-data.
 * Writes the transcribed text into transcript_out (up to TRANSCRIPT_MAX bytes).
 * Returns true on success.
 *
 * Multipart form fields required by Whisper:
 *   file:  audio/wav  binary
 *   model: "whisper-1"
 */
static bool stt_transcribe(const uint8_t *wav, size_t wav_len,
                            const char *api_key,
                            char *transcript_out)
{
    transcript_out[0] = '\0';

    /* Multipart boundary — safe because WAV binary data is opaque */
    static const char BOUNDARY[] = "----TCMDEchoBoundary7f3a2b";

    /* Multipart parts (text header sections) */
    static const char MODEL_PART[] =
        "------TCMDEchoBoundary7f3a2b\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-1\r\n";
    static const char FILE_PART_HDR[] =
        "------TCMDEchoBoundary7f3a2b\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    static const char FINAL_BOUNDARY[] =
        "\r\n------TCMDEchoBoundary7f3a2b--\r\n";

    /* Calculate total content length WITHOUT allocating the full body */
    size_t total_len = strlen(MODEL_PART)
                     + strlen(FILE_PART_HDR)
                     + wav_len
                     + strlen(FINAL_BOUNDARY);

    char bearer[STT_KEY_MAX + 10];
    snprintf(bearer, sizeof(bearer), "Bearer %s", api_key);

    char content_type[80];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", BOUNDARY);

    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        esp_http_client_config_t cfg = {
            .url               = STT_URL,
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = 30000,  /* STT may take a few seconds */
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size       = 4096,
            .buffer_size_tx    = 4096,
            .keep_alive_enable = false,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "STT init failed");
            break;
        }

        esp_http_client_set_header(client, "Authorization", bearer);
        esp_http_client_set_header(client, "Content-Type", content_type);

        esp_err_t ret = esp_http_client_open(client, (int)total_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "STT open failed (attempt %d/3): %s",
                     attempt, esp_err_to_name(ret));
            esp_http_client_cleanup(client);
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        /* Write multipart body in streaming fashion — no need to buffer the whole thing */
        int sent = 0;
        bool write_ok = true;

        /* Write model part */
        int n = esp_http_client_write(client, (const char *)MODEL_PART, strlen(MODEL_PART));
        if (n > 0) sent += n; else write_ok = false;

        /* Write file part header */
        n = esp_http_client_write(client, (const char *)FILE_PART_HDR, strlen(FILE_PART_HDR));
        if (n > 0) sent += n; else write_ok = false;

        /* Write WAV data (binary) */
        n = esp_http_client_write(client, (const char *)wav, wav_len);
        if (n > 0) sent += n; else write_ok = false;

        /* Write final boundary */
        n = esp_http_client_write(client, (const char *)FINAL_BOUNDARY, strlen(FINAL_BOUNDARY));
        if (n > 0) sent += n; else write_ok = false;

        if (write_ok && sent == (int)total_len) {
            int64_t cl       = esp_http_client_fetch_headers(client);
            int     max_resp = MAX_BODY;
            if (cl > 0 && cl < max_resp) max_resp = (int)cl;

            char *resp = malloc(max_resp + 1);
            if (resp) {
                int total = 0;
                while (total < max_resp) {
                    int n = esp_http_client_read(client, resp + total, max_resp - total);
                    if (n <= 0) break;
                    total += n;
                }
                resp[total] = '\0';

                int status = esp_http_client_get_status_code(client);
                ESP_LOGI(TAG, "STT response HTTP %d: %.200s", status, resp);

                if (status == 200) {
                    ok = json_extract_str(resp, "text", transcript_out, TRANSCRIPT_MAX);
                }
                free(resp);
            }
        } else if (!write_ok) {
            ESP_LOGW(TAG, "STT write failed (attempt %d/3), sent=%d/%d",
                     attempt, sent, (int)total_len);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (!ok && attempt < 3) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }

    return ok;
}

/* ── Health check ────────────────────────────────────────────────────────── */

/* Last recorded WAV kept for download via debug HTTP server */
static uint8_t *s_last_wav     = NULL;
static size_t   s_last_wav_len = 0;

static void do_health_check(const char *token)
{
    atom_led_set(255, 255, 255);  /* white flash */

    char *body = NULL;
    int   len  = https_get_auth(HEALTH_URL, token, &body);
    if (body) free(body);

    if (len >= 0) {
        ESP_LOGI(TAG, "Health check OK");
        atom_led_set(0, 100, 0);
        atom_audio_beep_ok();
    } else {
        ESP_LOGW(TAG, "Health check FAILED");
        atom_led_set(100, 0, 0);
        atom_audio_beep_fail();
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
}

/* ── Voice query ─────────────────────────────────────────────────────────── */

static void do_voice_query(const char *token, char *conv_id)
{
    /* Hand off shared GPIO33 from speaker LRCK to microphone PDM clock. */
    atom_audio_deinit();
    atom_mic_init();

    /* Cyan: recording */
    atom_led_set(0, 100, 100);

    /* Free previous WAV — kept around for HTTP download */
    if (s_last_wav) { free(s_last_wav); s_last_wav = NULL; s_last_wav_len = 0; }

    uint8_t *wav     = NULL;
    size_t   wav_len = atom_mic_record(&wav, BUTTON_GPIO, 4000);

    /* Hand GPIO33 back to speaker for status beeps and pair-code clips. */
    atom_mic_deinit();
    atom_audio_init();

    if (wav_len == 0 || !wav) {
        ESP_LOGW(TAG, "Voice record: empty or failed");
        atom_led_set(100, 0, 0);
        atom_audio_beep_fail();
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    /* Yellow: processing */
    atom_led_set(100, 80, 0);

    char stt_key[STT_KEY_MAX] = {0};
    if (!nvs_read_str(NVS_KEY_STT, stt_key, sizeof(stt_key))) {
        ESP_LOGW(TAG, "No STT key configured — voice query skipped");
        s_last_wav = wav; s_last_wav_len = wav_len;  /* keep for download */
        atom_led_set(100, 0, 0);
        atom_audio_beep_fail();
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    char transcript[TRANSCRIPT_MAX] = {0};
    bool stt_ok = stt_transcribe(wav, wav_len, stt_key, transcript);
    s_last_wav = wav; s_last_wav_len = wav_len;  /* keep for download */

    if (!stt_ok || transcript[0] == '\0') {
        ESP_LOGW(TAG, "STT failed or empty transcript");
        atom_led_set(100, 0, 0);
        atom_audio_beep_fail();
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    ESP_LOGI(TAG, "Transcript: %s", transcript);

    /* Build JSON body — escape any quotes in transcript */
    char json_body[TRANSCRIPT_MAX + 128];
    if (conv_id[0]) {
        snprintf(json_body, sizeof(json_body),
                 "{\"message\":\"%s\",\"conversationId\":\"%s\"}",
                 transcript, conv_id);
    } else {
        snprintf(json_body, sizeof(json_body),
                 "{\"message\":\"%s\"}", transcript);
    }

    ESP_LOGI(TAG, "Posting to Chat API: %s", json_body);
    
    char *resp   = NULL;
    int   status = https_post_json(CHAT_MSG_URL, token, json_body, &resp);

    ESP_LOGI(TAG, "Chat API response: HTTP %d", status);
    if (resp) ESP_LOGI(TAG, "Response body: %.256s", resp);

    if (status == 200 && resp) {
        char  new_conv[CONV_ID_MAX] = {0};
        char *assistant = malloc(MAX_BODY);

        json_extract_str(resp, "conversationId", new_conv, sizeof(new_conv));
        if (assistant) json_extract_str(resp, "content", assistant, MAX_BODY);

        if (new_conv[0] && strcmp(new_conv, conv_id) != 0) {
            strncpy(conv_id, new_conv, CONV_ID_MAX - 1);
            nvs_write_str(NVS_KEY_CONV, conv_id);
        }

        if (assistant && assistant[0])
            ESP_LOGI(TAG, "Assistant: %s", assistant);
        free(assistant);

        atom_led_set(0, 100, 0);
        atom_audio_beep_ok();
    } else {
        ESP_LOGW(TAG, "Chat API returned HTTP %d", status);
        atom_led_set(100, 0, 0);
        atom_audio_beep_fail();
    }

    if (resp) free(resp);
    vTaskDelay(pdMS_TO_TICKS(2000));
}

/* ── Pair code flow ──────────────────────────────────────────────────────── */

static void run_pair_code_flow(char *hw_token_out)
{
    while (true) {
        /* Fetch a new pair code */
        char *body = NULL;
        if (https_get_simple(PAIR_ENDPOINT, &body) < 0 || !body) {
            ESP_LOGW(TAG, "Failed to fetch pair code — retrying in 10 s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        char pair_code[16]  = {0};
        char pair_token[512] = {0};
        json_extract_str(body, "pairCode",  pair_code,  sizeof(pair_code));
        json_extract_str(body, "pairToken", pair_token, sizeof(pair_token));
        free(body);

        if (!pair_code[0] || !pair_token[0]) {
            ESP_LOGW(TAG, "Malformed pair response — retrying in 10 s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        ESP_LOGI(TAG, "Pair code: %s", pair_code);
        atom_led_set(200, 200, 200);  /* white: waiting for user to authorize */

        /* Speak the pair code */
        speak_pair_code(pair_code);

        /* Poll /pair/lookup until authorized, timed out, or button pressed */
        int polls_since_speech = 0;

        for (int poll = 0; poll < PAIR_MAX_POLLS; poll++) {
            /* Delay between polls, checking button every 100 ms */
            for (int t = 0; t < PAIR_POLL_INTERVAL_MS / 100; t++) {
                btn_press_t btn = poll_button();
                if (btn != BTN_NONE) {
                    /* Any press while pairing → re-speak pair code */
                    speak_pair_code(pair_code);
                    polls_since_speech = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            /* Re-speak periodically */
            polls_since_speech++;
            if (polls_since_speech >= PAIR_RESPEECH_INTERVAL) {
                speak_pair_code(pair_code);
                polls_since_speech = 0;
            }

            char *lookup_body = NULL;
            int   len = https_get_auth(PAIR_LOOKUP_URL, pair_token, &lookup_body);

            if (len >= 0 && lookup_body) {
                char token[HW_TOKEN_MAX] = {0};
                if (json_extract_str(lookup_body, "token", token, sizeof(token))) {
                    free(lookup_body);
                    ESP_LOGI(TAG, "Pair authorized — token received");
                    nvs_write_str(NVS_KEY_TOKEN, token);
                    strncpy(hw_token_out, token, HW_TOKEN_MAX - 1);
                    return;  /* success */
                }
                free(lookup_body);
            }
        }

        /* Timed out — loop back to get a fresh pair code */
        ESP_LOGW(TAG, "Pair code timed out — fetching new code");
    }
}

/* ── Debug HTTP server (WAV download) ────────────────────────────────────── */

static esp_err_t dbg_wav_handler(httpd_req_t *req)
{
    if (!s_last_wav || s_last_wav_len == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                            "No recording yet. Long-press the button first.");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"recording.wav\"");
    httpd_resp_send(req, (const char *)s_last_wav, (ssize_t)s_last_wav_len);
    return ESP_OK;
}

static const char s_dbg_index_html[] =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ATOM Echo Debug</title>"
    "<style>body{font-family:system-ui;background:#111;color:#eee;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh}"
    ".c{text-align:center}a{color:#6cf;font-size:1.3rem}</style></head>"
    "<body><div class='c'><h1>ATOM Echo Debug</h1>"
    "<p><a href='/wav'>Download last recording (WAV)</a></p>"
    "<p style='color:#888'>Long-press the button to record, then click above.</p>"
    "</div></body></html>";

static esp_err_t dbg_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_dbg_index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_debug_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t idx = { .uri = "/",    .method = HTTP_GET, .handler = dbg_index_handler };
        httpd_uri_t wav = { .uri = "/wav", .method = HTTP_GET, .handler = dbg_wav_handler   };
        httpd_register_uri_handler(server, &idx);
        httpd_register_uri_handler(server, &wav);

        /* Log device IP so user knows where to connect */
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            ESP_LOGI(TAG, "Debug server: http://" IPSTR "/wav", IP2STR(&ip.ip));
        } else {
            ESP_LOGI(TAG, "Debug HTTP server started on port 80");
        }
    } else {
        ESP_LOGW(TAG, "Failed to start debug HTTP server");
    }
}

/* ── Main entry point ────────────────────────────────────────────────────── */

void tcmd_atom_echo_run(void)
{
    /* ── Hardware init ────────────────────────────────────────────────────── */
    atom_led_init();
    atom_led_set(100, 80, 0);  /* yellow: booting */

    atom_audio_init();

    /* Configure button GPIO with internal pull-up */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        /* GPIO 39 is input-only on ESP32-PICO-D4 — no internal pull-up/down */
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* ── WiFi ─────────────────────────────────────────────────────────────── */
    if (!wifi_has_stored_credentials()) {
        atom_led_set(0, 0, 48);   /* dim blue: provisioning */
        /* SoftAP provisioning — this function never returns normally;
         * it calls esp_restart() after saving credentials. */
        run_softap_provisioning();
    }

    if (wifi_connect() != ESP_OK) {
        atom_led_set(100, 0, 0);  /* red: WiFi failed */
        atom_audio_beep_fail();
        ESP_LOGE(TAG, "WiFi connect failed — halting");
        vTaskSuspend(NULL);
    }

    atom_led_set(0, 60, 0);   /* green: connected */
    vTaskDelay(pdMS_TO_TICKS(1000));
    atom_led_off();

    /* Start debug HTTP server for WAV downloads */
    start_debug_http_server();

    /* ── NVS: read persistent state ───────────────────────────────────────── */
    static char s_hw_token[HW_TOKEN_MAX]  = {0};
    static char s_conv_id[CONV_ID_MAX]    = {0};

    bool have_token = nvs_read_str(NVS_KEY_TOKEN, s_hw_token, sizeof(s_hw_token));
    nvs_read_str(NVS_KEY_CONV, s_conv_id, sizeof(s_conv_id));

    /* ── STT key check ────────────────────────────────────────────────────── */
    /* If no STT key is stored, start the provisioning page again so the user
     * can enter it alongside WiFi credentials (or just the key if WiFi is OK). */
    char stt_check[STT_KEY_MAX] = {0};
    if (!nvs_read_str(NVS_KEY_STT, stt_check, sizeof(stt_check))) {
        ESP_LOGW(TAG, "No STT key — launching config page");
        /* Flash orange to indicate config needed */
        atom_led_set(100, 40, 0);
        run_softap_provisioning();
        /* run_softap_provisioning calls esp_restart() — never reaches here */
    }

    /* ── Pair code flow ───────────────────────────────────────────────────── */
    if (!have_token) {
        run_pair_code_flow(s_hw_token);
    }

    /* ── Idle loop ────────────────────────────────────────────────────────── */
    atom_led_set(20, 0, 20);  /* dim purple: idle/paired */

    ESP_LOGI(TAG, "ATOM Echo ready. Short press = health check, "
                  "long press = voice query.");

    while (true) {
        btn_press_t btn = poll_button();

        if (btn == BTN_SHORT) {
            do_health_check(s_hw_token);
            atom_led_set(20, 0, 20);  /* restore idle colour */

        } else if (btn == BTN_LONG) {
            do_voice_query(s_hw_token, s_conv_id);
            atom_led_set(20, 0, 20);  /* restore idle colour */
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
