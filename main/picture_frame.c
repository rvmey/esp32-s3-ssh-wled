/*
 * picture_frame.c
 *
 * TriggerCMD Picture Frame firmware loop for the Guition JC3248W535.
 *
 * Boot sequence:
 *   1. screen_init()
 *   2. WiFi — Improv-WiFi BLE provisioning if no stored credentials.
 *   3. User JWT — obtained via pair code flow:
 *        GET /pair?model=TCMDSCREEN → {pairCode, pairToken}
 *        Display code; poll GET /pair/lookup every 5 s (up to 10 min).
 *        On authorisation, token is saved to NVS and device reboots.
 *        On timeout, a fresh pair code is fetched automatically.
 *   4. Provisioning — POST /api/computer/save with name TCMDSCREEN-<MAC>,
 *      receiving back a computer ID stored in NVS.
 *   5. Command sync — GET /api/command/list, then POST /api/command/save
 *      for any commands from picture_frame_commands.json not yet online.
 *   6. Socket.IO connect; subscribe via Sails.io virtual GET
 *        /api/computer/subscribeToFunRoom?roomName=<computer_id>
 *   7. Event loop — dispatches "message" events (text/color/textcolor/
 *      fontsize/landscape/portrait/jpeg) to screen functions; reports back
 *      via POST /api/run/save.
 */

#include "picture_frame.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "wifi_manager.h"
#include "improv_wifi.h"
#include "screen_control.h"
#include "socketio.h"
#include "http_pf_config.h"
#include "triggercmd_ca.h"   /* embedded Go Daddy Root G2 cert for triggercmd.com */
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"


static const char *TAG = "pf";

/* ── Server URL base (change to dev server as needed) ──────────────────── */
/*
 * Examples:
 *   "https://www.triggercmd.com"   — production
 *   "https://xxxx.ngrok-free.app"  — ngrok HTTPS tunnel
 *   "http://192.168.1.50:3000"     — local dev server (no TLS)
 */
static const char *TCMD_BASE_URL = "http://192.168.86.28:1337";

/* Strip scheme prefix for use in display text */
static const char *tcmd_display_host(void)
{
    if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) return TCMD_BASE_URL + 8;
    if (strncmp(TCMD_BASE_URL, "http://",  7) == 0) return TCMD_BASE_URL + 7;
    return TCMD_BASE_URL;
}

/* ── NVS helpers ────────────────────────────────────────────────────────── */

#define NVS_NS          "pf_cfg"
#define NVS_KEY_TOKEN   "hw_token"
#define NVS_KEY_COMPID  "computer_id"

#define HW_TOKEN_MAX_LEN    513   /* 512 payload + NUL */
#define COMPUTER_ID_MAX_LEN  33   /* 32 payload + NUL  */
#define COMPUTER_NAME_LEN    32   /* "TCMDSCREEN-AABBCCDDEEFF" + NUL */

/* ── Module-level statics shared with event handler ────────────────────── */
static char s_hw_token[HW_TOKEN_MAX_LEN]      = {0};
static char s_computer_id[COMPUTER_ID_MAX_LEN] = {0};

/* Pending run/save — set by the WS event task, consumed by the main loop */
static char          s_pending_run_id[33] = {0};
static volatile bool s_pending_run        = false;

/* Pending JPEG URL — set by the WS event task, consumed by the main loop */
static char          s_pending_jpeg_url[512] = {0};
static volatile bool s_pending_jpeg          = false;

/* Cached compressed JPEG — kept in PSRAM so orientation changes can redraw
 * without re-downloading.  Freed when a non-jpeg command replaces the display. */
static uint8_t      *s_jpeg_cache     = NULL;
static int           s_jpeg_cache_len = 0;
static volatile bool s_pending_jpeg_redraw = false;

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
        .url      = url,
        .method   = HTTP_METHOD_GET,
        .timeout_ms = 15000,
    };
    /* TLS: embedded GoDaddy cert for prod; crt_bundle for other HTTPS; plain HTTP needs nothing */
    if (strstr(TCMD_BASE_URL, "triggercmd.com")) {
        cfg.cert_pem = TRIGGERCMD_CA_PEM;
    } else if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

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

/*
 * HTTPS POST with url-encoded form body and Bearer token auth.
 * body_form is a pre-encoded "key=val&key2=val2" string.
 * Returns HTTP status code, or -1 on network/TLS error.
 * Response body written into malloc'd *resp_body (caller frees), or NULL on failure.
 */
static int https_post_form(const char *url, const char *token,
                           const char *body_form, char **resp_body)
{
    if (resp_body) *resp_body = NULL;

    char bearer[560];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    int body_len = (int)strlen(body_form);

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };
    if (strstr(TCMD_BASE_URL, "triggercmd.com")) {
        cfg.cert_pem = TRIGGERCMD_CA_PEM;
    } else if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");

    esp_err_t ret = esp_http_client_open(client, body_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "https_post_form open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return -1;
    }

    int written = esp_http_client_write(client, body_form, body_len);
    if (written < 0) {
        ESP_LOGE(TAG, "https_post_form write failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    int64_t cl = esp_http_client_fetch_headers(client);
    int max_resp = 2048;
    if (cl > 0 && cl < max_resp) max_resp = (int)cl;

    if (resp_body) {
        char *buf = malloc(max_resp + 1);
        if (buf) {
            int total = 0;
            while (total < max_resp) {
                int n = esp_http_client_read(client, buf + total, max_resp - total);
                if (n <= 0) break;
                total += n;
            }
            buf[total] = '\0';
            *resp_body = buf;
        }
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

/*
 * HTTPS GET without auth headers. Returns body length or -1 on failure.
 * Caller must free *body.
 */
static int https_get_simple(const char *url, char **body)
{
    *body = NULL;

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 15000,
    };
    /* TLS: embedded GoDaddy cert for prod; crt_bundle for other HTTPS; plain HTTP needs nothing */
    if (strstr(TCMD_BASE_URL, "triggercmd.com")) {
        cfg.cert_pem = TRIGGERCMD_CA_PEM;
    } else if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "https_get_simple open failed: %s", esp_err_to_name(ret));
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
        ESP_LOGE(TAG, "https_get_simple %s → HTTP %d", url, status);
        free(buf);
        return -1;
    }

    *body = buf;
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

/*
 * Two-step nested JSON extractor: finds "outer":{...} then extracts key
 * inside that block.  e.g. json_extract_nested("...", "data", "id", ...).
 */
static bool json_extract_nested(const char *json, const char *outer,
                                 const char *key, char *out, size_t out_sz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", outer);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '{') return false;
    /* find closing brace — simple scan, not bracket-aware beyond one level */
    const char *end = strchr(p, '}');
    if (!end) return false;
    /* extract key from the sub-object */
    char sub[256];
    size_t sub_len = (size_t)(end - p + 1);
    if (sub_len >= sizeof(sub)) sub_len = sizeof(sub) - 1;
    memcpy(sub, p, sub_len);
    sub[sub_len] = '\0';
    return json_extract_str(sub, key, out, out_sz);
}

/* ── URL percent-encoding helpers ──────────────────────────────────────── */
/*
 * Append a URL-encoded version of src into dst (remaining capacity rem).
 * Spaces → '+', other non-unreserved bytes → %XX.
 * Returns pointer past the last written byte.
 */
static char *url_encode_append(char *dst, size_t rem, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    while (*src && rem > 1) {
        unsigned char c = (unsigned char)*src++;
        if (c == ' ') {
            if (rem > 1) { *dst++ = '+'; rem--; }
        } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *dst++ = (char)c; rem--;
        } else {
            if (rem > 3) {
                *dst++ = '%';
                *dst++ = hex[c >> 4];
                *dst++ = hex[c & 0xF];
                rem -= 3;
            }
        }
    }
    return dst;
}

/* ── Embedded command definitions ──────────────────────────────────────── */

typedef struct {
    const char *trigger;
    const char *voice;
    const char *allow_params;   /* "true" or "false" */
    const char *mcp_desc;
    const char *icon;           /* UTF-8 emoji */
} pf_cmd_t;

static const pf_cmd_t s_pf_cmds[] = {
    { "text",      "text",      "true",  "Update the display text. Example: 'text Hello world!'",           "\xF0\x9F\x93\x9D" /* 📝 */ },
    { "color",     "color",     "true",  "Change the display color. Example: 'color red' or 'color #FF0000'", "\xF0\x9F\x94\xA4" /* 🔤 */ },
    { "textcolor", "textcolor", "true",  "Change the text color. Example: 'textcolor blue' or 'textcolor #0000FF'", "\xF0\x9F\x8E\xA8" /* 🎨 */ },
    { "fontsize",  "fontsize",  "true",  "Change the font size. Example: 'fontsize 3'",                     "\xF0\x9F\x94\xA1" /* 🔡 */ },
    { "landscape", "landscape", "false", "Set the display to landscape orientation.",                        "\xE2\x86\x94\xEF\xB8\x8F" /* ↔️ */ },
    { "portrait",  "portrait",  "false", "Set the display to portrait orientation.",                         "\xE2\x86\x95\xEF\xB8\x8F" /* ↕️ */ },
    { "jpeg",      "jpeg",      "true",  "Display a JPEG image. Example: 'jpeg https://example.com/image.jpg'", "\xF0\x9F\x96\xBC\xEF\xB8\x8F" /* 🖼️ */ },
};
#define PF_CMD_COUNT  (sizeof(s_pf_cmds) / sizeof(s_pf_cmds[0]))

/* ── Color parser ──────────────────────────────────────────────────────── */

static bool parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* skip leading whitespace */
    while (*s == ' ') s++;

    /* Named colours */
    struct { const char *name; uint8_t r, g, b; } named[] = {
        {"red",     255,   0,   0}, {"green",   0, 200,   0},
        {"blue",      0,   0, 255}, {"white",  255, 255, 255},
        {"black",     0,   0,   0}, {"yellow", 255, 255,   0},
        {"cyan",      0, 255, 255}, {"magenta",255,   0, 255},
        {"orange",  255, 165,   0}, {"purple", 128,   0, 128},
        {"pink",    255, 105, 180}, {"gray",   128, 128, 128},
        {"grey",    128, 128, 128},
    };
    for (int i = 0; i < (int)(sizeof(named)/sizeof(named[0])); i++) {
        if (strcasecmp(s, named[i].name) == 0) {
            *r = named[i].r; *g = named[i].g; *b = named[i].b;
            return true;
        }
    }

    /* Hex: #RRGGBB or RRGGBB */
    if (*s == '#') s++;
    if (strlen(s) >= 6) {
        unsigned int rv, gv, bv;
        if (sscanf(s, "%02x%02x%02x", &rv, &gv, &bv) == 3) {
            *r = (uint8_t)rv; *g = (uint8_t)gv; *b = (uint8_t)bv;
            return true;
        }
    }
    return false;
}

/* ── Socket.IO event handler ────────────────────────────────────────────── */

static void pf_event_handler(const char *event_name,
                              const char *payload_json,
                              void       *ctx)
{
    (void)ctx;

    ESP_LOGI(TAG, "pf_event: name='%s' payload=%.200s", event_name, payload_json);

    if (strcmp(event_name, "message") != 0) return;

    static char s_trigger[64];
    static char s_id[32];
    static char s_params[256];

    s_trigger[0] = s_id[0] = s_params[0] = '\0';
    json_extract_str(payload_json, "trigger", s_trigger, sizeof(s_trigger));
    json_extract_str(payload_json, "id",      s_id,      sizeof(s_id));
    json_extract_str(payload_json, "params",  s_params,  sizeof(s_params));

    ESP_LOGI(TAG, "message dispatch: trigger='%s' id='%s' params='%s'",
             s_trigger, s_id, s_params);

    if (strcmp(s_trigger, "text") == 0) {
        screen_draw_text(s_params[0] ? s_params : " ");

    } else if (strcmp(s_trigger, "color") == 0) {
        uint8_t r = 0, g = 0, b = 0;
        if (parse_color(s_params, &r, &g, &b)) {
            screen_set_color(r, g, b);
        } else {
            ESP_LOGW(TAG, "color: unrecognised '%s'", s_params);
        }

    } else if (strcmp(s_trigger, "textcolor") == 0) {
        uint8_t r = 255, g = 255, b = 255;
        if (parse_color(s_params, &r, &g, &b)) {
            screen_set_text_color(r, g, b);
        } else {
            ESP_LOGW(TAG, "textcolor: unrecognised '%s'", s_params);
        }

    } else if (strcmp(s_trigger, "fontsize") == 0) {
        int scale = atoi(s_params);
        if (scale < 1) scale = 1;
        if (scale > 4) scale = 4;
        screen_set_font_scale(scale);

    } else if (strcmp(s_trigger, "landscape") == 0) {
        screen_set_landscape(true);
        if (s_jpeg_cache) s_pending_jpeg_redraw = true;

    } else if (strcmp(s_trigger, "portrait") == 0) {
        screen_set_landscape(false);
        if (s_jpeg_cache) s_pending_jpeg_redraw = true;

    } else if (strcmp(s_trigger, "jpeg") == 0) {
        if (s_params[0]) {
            /* Trim leading whitespace from URL */
            const char *url = s_params;
            while (*url == ' ') url++;
            strncpy(s_pending_jpeg_url, url, sizeof(s_pending_jpeg_url) - 1);
            s_pending_jpeg_url[sizeof(s_pending_jpeg_url) - 1] = '\0';
            s_pending_jpeg = true;
        } else {
            ESP_LOGW(TAG, "jpeg: no URL in params");
            return;  /* don't report run/save for empty command */
        }

    } else {
        ESP_LOGW(TAG, "message: unknown trigger '%s'", s_trigger);
        return;
    }

    /* Queue run/save to the main loop — do NOT call https_post_form here.
     * This callback runs in the esp_websocket_client internal task; blocking
     * it with an HTTP request prevents ping/pong processing and causes the
     * server to close the WebSocket connection. */
    if (s_id[0] && s_computer_id[0]) {
        strncpy(s_pending_run_id, s_id, sizeof(s_pending_run_id) - 1);
        s_pending_run_id[sizeof(s_pending_run_id) - 1] = '\0';
        s_pending_run = true;
    }
}

/* ── JPEG download + decode + display ───────────────────────────────────── */
/*
 * Downloads a JPEG from url, decodes it to RGB565, and blits it to the
 * screen.  All large allocations use PSRAM so heap fragmentation is avoided.
 * The JPEG input buffer is capped at 512 KB; most photos will be much smaller.
 */
#define JPEG_DL_MAX   (512 * 1024)   /* max JPEG download size */

/*
 * Decode compressed JPEG bytes (buf, len) to RGB565 and blit to the screen.
 * Called both after a fresh download and when re-blitting on orientation change.
 */
static void decode_and_show_jpeg(const uint8_t *buf, int len)
{
    esp_jpeg_image_cfg_t info_cfg = {
        .indata      = (uint8_t *)buf,
        .indata_size = (uint32_t)len,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&info_cfg, &info) != ESP_OK || info.output_len == 0) {
        ESP_LOGE(TAG, "jpeg: get_image_info failed");
        screen_draw_text("Image decode\nfailed");
        return;
    }
    ESP_LOGI(TAG, "jpeg: image %ux%u, output_len=%zu", info.width, info.height, info.output_len);

    uint8_t *rgb565_buf = heap_caps_malloc(info.output_len,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565_buf) rgb565_buf = malloc(info.output_len);
    if (!rgb565_buf) {
        ESP_LOGE(TAG, "jpeg: no memory for %zu byte RGB565 buffer", info.output_len);
        screen_draw_text("Image decode\nfailed");
        return;
    }

    esp_jpeg_image_cfg_t dec_cfg = {
        .indata      = (uint8_t *)buf,
        .indata_size = (uint32_t)len,
        .outbuf      = rgb565_buf,
        .outbuf_size = info.output_len,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t out = {0};
    esp_err_t dec_ret = esp_jpeg_decode(&dec_cfg, &out);
    if (dec_ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg: decode failed: %s", esp_err_to_name(dec_ret));
        free(rgb565_buf);
        screen_draw_text("Image decode\nfailed");
        return;
    }

    ESP_LOGI(TAG, "jpeg: decoded %ux%u → blitting", out.width, out.height);
    screen_draw_rgb565(rgb565_buf, (int)out.width, (int)out.height);
    free(rgb565_buf);
}

static void download_and_show_jpeg(const char *url)
{
    screen_draw_text("Loading image...");

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 15000,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "jpeg: http_client_init failed");
        screen_draw_text("Image load\nfailed");
        return;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg: HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        screen_draw_text("Image load\nfailed");
        return;
    }

    int64_t cl = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "jpeg: HTTP %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        screen_draw_text("Image load\nfailed");
        return;
    }

    int max_dl = JPEG_DL_MAX;
    if (cl > 0 && cl < max_dl) max_dl = (int)cl;

    uint8_t *jpeg_buf = heap_caps_malloc(max_dl, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buf) {
        /* fall back to internal heap for small content-lengths */
        jpeg_buf = malloc(max_dl);
    }
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "jpeg: no memory for %d byte download", max_dl);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        screen_draw_text("Image load\nfailed");
        return;
    }

    int total = 0;
    while (total < max_dl) {
        int n = esp_http_client_read(client, (char *)jpeg_buf + total, max_dl - total);
        if (n <= 0) break;
        total += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total == 0) {
        ESP_LOGE(TAG, "jpeg: empty response from %s", url);
        free(jpeg_buf);
        screen_draw_text("Image load\nfailed");
        return;
    }
    ESP_LOGI(TAG, "jpeg: downloaded %d bytes", total);

    /* Replace cache — keep compressed bytes for orientation-change redraws */
    if (s_jpeg_cache) { free(s_jpeg_cache); s_jpeg_cache = NULL; }
    s_jpeg_cache     = jpeg_buf;   /* take ownership — do NOT free */
    s_jpeg_cache_len = total;

    decode_and_show_jpeg(s_jpeg_cache, s_jpeg_cache_len);
}

/* ── Connection step: Socket.IO + subscribeToFunRoom ────────────────────── */

static esp_err_t connect_and_subscribe(void)
{
    screen_draw_text("Connecting to server...");

    /* __sails_io_sdk_version=0.11.0 in the handshake URL is how Sails 0.12.x
     * identifies a valid sails.io SDK client (checked in parseVirtualRequest). */
    char sio_url[256];
    if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        snprintf(sio_url, sizeof(sio_url),
                 "wss://%s/socket.io/?EIO=4&transport=websocket&__sails_io_sdk_version=0.11.0",
                 TCMD_BASE_URL + 8);
    } else {
        snprintf(sio_url, sizeof(sio_url),
                 "ws://%s/socket.io/?EIO=4&transport=websocket&__sails_io_sdk_version=0.11.0",
                 TCMD_BASE_URL + 7);   /* skip "http://" */
    }

    esp_err_t ret = socketio_connect(sio_url, s_hw_token, pf_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "socketio_connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Subscribe via Sails.io virtual GET over the active socket.
     * Append __sails_io_sdk_version to the path (Python client style). */
    char sub_path[192];
    snprintf(sub_path, sizeof(sub_path),
             "/api/computer/subscribeToFunRoom?roomName=%s&__sails_io_sdk_version=0.11.0",
             s_computer_id);
    socketio_send_vget(sub_path, s_hw_token);

    screen_draw_text("Connected!\nWaiting for\ncommands...");
    return ESP_OK;
}

/* ── Main entry point ───────────────────────────────────────────────────── */

void picture_frame_run(void)
{
    /* Initialise display first — screen_init() creates s_draw_mutex which all
     * screen_draw_*() helpers require.  Must happen before any screen call. */
    screen_init();

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
    bool have_token   = nvs_read_str(NVS_KEY_TOKEN,  s_hw_token,   sizeof(s_hw_token));
    bool have_comp_id = nvs_read_str(NVS_KEY_COMPID, s_computer_id, sizeof(s_computer_id));

    /* ── Pair code flow — runs until a user JWT is obtained ─────────────── */
    if (!have_token) {
        while (true) {  /* pair_loop — retries on network failure or 10-min timeout */

            char *pair_body = NULL;
            char pair_url[192];
            snprintf(pair_url, sizeof(pair_url),
                     "%s/pair?model=TCMDSCREEN", TCMD_BASE_URL);
            int pair_len = https_get_simple(pair_url, &pair_body);

            if (pair_len <= 0 || !pair_body) {
                ESP_LOGE(TAG, "GET /pair failed — retrying in 10s");
                screen_draw_text("Pairing failed\nRetrying...");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }

            char pair_code[8]    = {0};
            char pair_token[768] = {0};
            json_extract_str(pair_body, "pairCode",  pair_code,  sizeof(pair_code));
            json_extract_str(pair_body, "pairToken", pair_token, sizeof(pair_token));
            free(pair_body);

            if (pair_code[0] == '\0' || pair_token[0] == '\0') {
                ESP_LOGE(TAG, "GET /pair: missing pairCode/pairToken — retrying in 10s");
                screen_draw_text("Pairing failed\nRetrying...");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }

            ESP_LOGI(TAG, "Pair code: %s", pair_code);

            char pair_disp[192];
            snprintf(pair_disp, sizeof(pair_disp),
                     "Visit %s\nSign in -> click name\n"
                     "Click Pair -> enter:\n%s", tcmd_display_host(), pair_code);
            screen_draw_text(pair_disp);
            http_pf_config_start(pair_code);

            char lookup_url[900];
            snprintf(lookup_url, sizeof(lookup_url),
                     "%s/pair/lookup?token=%s", TCMD_BASE_URL, pair_token);

            bool paired = false;
            for (int i = 0; i < 120 && !paired; i++) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                char *lk_body = NULL;
                int lk_len = https_get_simple(lookup_url, &lk_body);
                if (lk_len > 0 && lk_body) {
                    char tok_new[HW_TOKEN_MAX_LEN] = {0};
                    if (json_extract_str(lk_body, "token", tok_new, sizeof(tok_new)) &&
                            tok_new[0] != '\0') {
                        nvs_write_str(NVS_KEY_TOKEN, tok_new);
                        ESP_LOGI(TAG, "Paired — token saved; rebooting");
                        paired = true;
                    }
                    free(lk_body);
                }
            }

            http_pf_config_stop();

            if (paired) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }

            ESP_LOGI(TAG, "Pair code timed out — fetching new code");
        }
    }

    /* ── Create computer if not already provisioned ──────────────────────── */
    if (!have_comp_id) {
        /* Build a unique computer name from the WiFi base MAC */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char computer_name[COMPUTER_NAME_LEN];
        snprintf(computer_name, sizeof(computer_name),
                 "TCMDSCREEN-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        ESP_LOGI(TAG, "Creating computer: %s", computer_name);
        screen_draw_text("Creating computer...");

        char save_url[192];
        snprintf(save_url, sizeof(save_url), "%s/api/computer/save", TCMD_BASE_URL);

        /* Body: name=TCMDSCREEN-AABBCCDDEEFF (no special encoding needed) */
        char form[64];
        snprintf(form, sizeof(form), "name=%s", computer_name);

        char *resp = NULL;
        int status = https_post_form(save_url, s_hw_token, form, &resp);
        if (status >= 200 && status < 300 && resp) {
            char cid[COMPUTER_ID_MAX_LEN] = {0};
            if (json_extract_nested(resp, "data", "id", cid, sizeof(cid)) && cid[0]) {
                strncpy(s_computer_id, cid, sizeof(s_computer_id) - 1);
                nvs_write_str(NVS_KEY_COMPID, s_computer_id);
                ESP_LOGI(TAG, "computer_id stored: %s", s_computer_id);

                char msg[64];
                snprintf(msg, sizeof(msg), "Ready!\n%s", computer_name);
                screen_draw_text(msg);
            } else {
                ESP_LOGE(TAG, "computer/save: could not parse data.id from: %s", resp);
                free(resp);
                screen_draw_text("Provision failed\nRetrying in 10s");
                vTaskDelay(pdMS_TO_TICKS(10000));
                esp_restart();
            }
        } else {
            ESP_LOGE(TAG, "computer/save → HTTP %d", status);
            if (resp) free(resp);
            screen_draw_text("Provision failed\nRetrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            esp_restart();
        }
        if (resp) free(resp);

        vTaskDelay(pdMS_TO_TICKS(1500));

        /* ── Sync commands ────────────────────────────────────────────────── */
        screen_draw_text("Syncing commands...");

        char list_url[192];
        snprintf(list_url, sizeof(list_url),
                 "%s/api/command/list?computer_id=%s",
                 TCMD_BASE_URL, s_computer_id);

        char *list_body = NULL;
        int list_len = https_get_auth(list_url, s_hw_token, &list_body);
        /* list_body may be NULL if request fails — we'll still attempt adds */

        char cmd_url[192];
        snprintf(cmd_url, sizeof(cmd_url), "%s/api/command/save", TCMD_BASE_URL);

        for (size_t i = 0; i < PF_CMD_COUNT; i++) {
            const pf_cmd_t *cmd = &s_pf_cmds[i];

            /* Check if trigger name already exists in online list */
            bool found = false;
            if (list_len > 0 && list_body) {
                /* Search for "name":"<trigger>" in the JSON response */
                char needle[96];
                snprintf(needle, sizeof(needle), "\"name\":\"%s\"", cmd->trigger);
                if (strstr(list_body, needle)) found = true;
            }

            if (found) {
                ESP_LOGI(TAG, "cmd '%s' already online — skip", cmd->trigger);
                continue;
            }

            /* Build url-encoded form body */
            char body[512];
            char *p    = body;
            char *bend = body + sizeof(body);

#define APPEND_FIELD(k, v) \
            p = url_encode_append(p, (size_t)(bend - p), (k)); \
            if (p < bend) *p++ = '='; \
            p = url_encode_append(p, (size_t)(bend - p), (v)); \
            if (p < bend) *p++ = '&';

            APPEND_FIELD("name",               cmd->trigger)
            APPEND_FIELD("computer",           s_computer_id)
            APPEND_FIELD("voice",              cmd->voice)
            APPEND_FIELD("voiceReply",         "")
            APPEND_FIELD("allowParams",        cmd->allow_params)
            APPEND_FIELD("mcpToolDescription", cmd->mcp_desc)
            APPEND_FIELD("icon",               cmd->icon)
#undef APPEND_FIELD
            /* remove trailing '&' */
            if (p > body && *(p-1) == '&') p--;
            *p = '\0';

            int cs = https_post_form(cmd_url, s_hw_token, body, NULL);
            ESP_LOGI(TAG, "cmd/save '%s' → HTTP %d", cmd->trigger, cs);
        }

        if (list_body) free(list_body);
    }

    /* ── Connect + subscribe loop ────────────────────────────────────────── */
    while (true) {
        esp_err_t ret = connect_and_subscribe();
        if (ret != ESP_OK) {
            screen_draw_text("Server connect\nfailed\nRetrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            socketio_disconnect();
            continue;
        }

        TickType_t last_ping_tick = xTaskGetTickCount();

        while (true) {
            /* Download + display JPEG from main task — blocking HTTP + decode */
            if (s_pending_jpeg_redraw) {
                s_pending_jpeg_redraw = false;
                if (s_jpeg_cache) {
                    decode_and_show_jpeg(s_jpeg_cache, s_jpeg_cache_len);
                }
            }

            if (s_pending_jpeg) {
                s_pending_jpeg = false;
                char jpeg_url[512];
                strncpy(jpeg_url, s_pending_jpeg_url, sizeof(jpeg_url) - 1);
                jpeg_url[sizeof(jpeg_url) - 1] = '\0';
                download_and_show_jpeg(jpeg_url);
            }

            /* Post run/save from the main task — never from the WS callback task */
            if (s_pending_run) {
                s_pending_run = false;   /* clear before HTTP so new events can re-queue */
                char run_url[192];
                snprintf(run_url, sizeof(run_url), "%s/api/run/save", TCMD_BASE_URL);
                char form[256];
                char *p   = form;
                char *end = form + sizeof(form);
                p = url_encode_append(p, (size_t)(end - p), "status");
                if (p < end) *p++ = '=';
                p = url_encode_append(p, (size_t)(end - p), "Command ran");
                if (p < end) *p++ = '&';
                p = url_encode_append(p, (size_t)(end - p), "computer");
                if (p < end) *p++ = '=';
                p = url_encode_append(p, (size_t)(end - p), s_computer_id);
                if (p < end) *p++ = '&';
                p = url_encode_append(p, (size_t)(end - p), "command");
                if (p < end) *p++ = '=';
                p = url_encode_append(p, (size_t)(end - p), s_pending_run_id);
                *p = '\0';
                int http_status = https_post_form(run_url, s_hw_token, form, NULL);
                ESP_LOGI(TAG, "run/save \u2192 HTTP %d", http_status);
            }

            vTaskDelay(pdMS_TO_TICKS(200));   /* poll every 200 ms */

            /* EIO keepalive: send "2" ping every 20 s so the server doesn't
             * close the socket after its 60-second pingTimeout. */
            if ((xTaskGetTickCount() - last_ping_tick) >= pdMS_TO_TICKS(20000)) {
                socketio_send_eio_ping();
                last_ping_tick = xTaskGetTickCount();
            }

            if (!socketio_connected()) {
                ESP_LOGW(TAG, "Socket.IO disconnected — reconnecting");
                screen_draw_text("Reconnecting...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                socketio_disconnect();
                break;
            }
        }
    }
}
