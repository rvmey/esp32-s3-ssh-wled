/*
 * tcmd_atoms3_lite.c — TRIGGERcmd firmware for M5Stack AtomS3 Lite.
 *
 * Boot flow:
 *   1. Init RGB LED (GPIO 35) + configure button (GPIO 41, active-low)
 *   2. WiFi: if no stored credentials → SoftAP provisioning page
 *      (collects SSID/password, secondary networks, and Bookmark URL)
 *   3. wifi_connect()
 *   4. Start HTTP config server on port 80
 *   5. Pair code loop: GET /pair?model=TCMDATOMS3 → display code via HTTP server
 *      Poll /pair/lookup every 5 s (up to 10 min); on authorise → save token, restart
 *   6. Provision computer: POST /api/computer/save → receive computer_id
 *   7. Sync commands to TRIGGERcmd account
 *   8. Connect Socket.IO, subscribe to computer room
 *   9. Event loop:
 *        - "color" command  → set LED color
 *        - "off" command    → turn LED off
 *        - "reboot" command → restart device
 *        - Button short press → call Bookmark URL (HTTP GET, no cert check)
 *        - Periodic EIO ping to keep Socket.IO alive
 *        - run/save reports via Socket.IO virtual POST
 *
 * NVS namespace: "asl_cfg"
 *   hw_token    — TRIGGERcmd user JWT (512 bytes max)
 *   computer_id — device identifier (32 bytes max)
 *   bookmark    — Single-click Bookmark URL  (256 bytes max)
 *   bookmark2   — Long-press Bookmark URL   (256 bytes max)
 *   bookmark3   — Double-click Bookmark URL (256 bytes max)
 *   bookmark4   — Triple-click Bookmark URL (256 bytes max)
 *   led_r/g/b   — Last set LED color, restored on boot
 */

#include "tcmd_atoms3_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "wifi_manager.h"
#include "atoms3_led.h"
#include "socketio.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

extern const char g_firmware_version[];

#define TAG              "atoms3_lite"

#define BUTTON_GPIO      41        /* active-low, internal pull-up */
#define LONG_PRESS_MS    2000
#define CLICK_WINDOW_MS   350   /* max gap between clicks to count as multi-click */

#define NVS_NS           "asl_cfg"
#define NVS_KEY_TOKEN    "hw_token"
#define NVS_KEY_COMPID   "computer_id"
#define NVS_KEY_BOOKMARK  "bookmark"
#define NVS_KEY_BOOKMARK2 "bookmark2"
#define NVS_KEY_BOOKMARK3 "bookmark3"
#define NVS_KEY_BOOKMARK4 "bookmark4"
#define NVS_KEY_LED_R    "led_r"
#define NVS_KEY_LED_G    "led_g"
#define NVS_KEY_LED_B    "led_b"

/* Core2 remote trigger */
#define NVS_KEY_CORE2_IP   "core2_ip"
#define NVS_KEY_CORE2_NAME "core2_name"
#define NVS_KEY_CMD1       "core2_cmd1"
#define NVS_KEY_PAR1       "core2_par1"
#define NVS_KEY_CMD2       "core2_cmd2"
#define NVS_KEY_PAR2       "core2_par2"
#define NVS_KEY_CMD3       "core2_cmd3"
#define NVS_KEY_PAR3       "core2_par3"
#define NVS_KEY_CMD4       "core2_cmd4"
#define NVS_KEY_PAR4       "core2_par4"

#define CORE2_IP_MAX     48
#define CORE2_NAME_MAX   64
#define CMD_NAME_MAX     64
#define CMD_PARAMS_MAX  128

#define TCMD_DISCOVER_PORT 5380

#define TCMD_BASE_URL    "https://www.triggercmd.com"
#define COMPUTER_MODEL   "TCMDATOMS3"

#define HTTP_TIMEOUT_MS  20000
#define MAX_BODY         65536

#define HW_TOKEN_MAX     513   /* 512 payload + NUL */
#define COMPUTER_ID_MAX   33   /* 32 payload + NUL  */
#define COMPUTER_NAME_MAX 28   /* "TCMDATOMS3-AABBCCDDEEFF" + NUL */
#define BOOKMARK_MAX     257   /* 256 payload + NUL */

#define PAIR_POLL_MS     5000
#define PAIR_MAX_POLLS   120   /* 10 minutes */

/* ── Module state ────────────────────────────────────────────────────────── */

static char s_hw_token[HW_TOKEN_MAX]         = {0};
static char s_computer_id[COMPUTER_ID_MAX]   = {0};
static char s_bookmark_url[BOOKMARK_MAX]      = {0};
static char s_bookmark_url2[BOOKMARK_MAX]     = {0};
static char s_bookmark_url3[BOOKMARK_MAX]     = {0};
static char s_bookmark_url4[BOOKMARK_MAX]     = {0};

/* Core2 remote trigger state */
static char s_core2_ip[CORE2_IP_MAX]       = {0};
static char s_core2_name[CORE2_NAME_MAX]   = {0};
static char s_core2_cmd1[CMD_NAME_MAX]     = {0};
static char s_core2_par1[CMD_PARAMS_MAX]   = {0};
static char s_core2_cmd2[CMD_NAME_MAX]     = {0};
static char s_core2_par2[CMD_PARAMS_MAX]   = {0};
static char s_core2_cmd3[CMD_NAME_MAX]     = {0};
static char s_core2_par3[CMD_PARAMS_MAX]   = {0};
static char s_core2_cmd4[CMD_NAME_MAX]     = {0};
static char s_core2_par4[CMD_PARAMS_MAX]   = {0};

/* Set by Socket.IO callback, consumed by main loop */
static volatile bool s_pending_color = false;
static uint8_t       s_pending_r = 0, s_pending_g = 0, s_pending_b = 0;
static volatile bool s_pending_off = false;
static volatile bool s_pending_reboot = false;

/* run/save: set by callback, consumed by main loop */
static char          s_pending_run_id[33] = {0};
static volatile bool s_pending_run = false;

/* HTTP config server state */
static httpd_handle_t s_cfg_server     = NULL;
static char           s_pair_code[8]   = {0};   /* empty = paired */

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

static bool nvs_read_u8(const char *key, uint8_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(h, key, &val);
    nvs_close(h);
    if (err == ESP_OK) { *out = val; return true; }
    return false;
}

static esp_err_t nvs_write_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

static void nvs_erase_key_local(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

/* ── URL / form helpers ──────────────────────────────────────────────────── */

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

static bool form_get_field(const char *body, const char *field,
                            char *out, size_t out_sz)
{
    size_t flen = strlen(field);
    const char *p = body;
    out[0] = '\0';
    while (p && *p) {
        if (strncmp(p, field, flen) == 0 && p[flen] == '=') {
            p += flen + 1;
            const char *end = strchr(p, '&');
            char encoded[512] = {0};
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

/* URL-encode src, appending to dst.  Returns pointer past last written byte. */
static char *url_encode_append(char *dst, size_t rem, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    while (*src && rem > 1) {
        unsigned char c = (unsigned char)*src++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *dst++ = (char)c;
            rem--;
        } else if (c == ' ') {
            *dst++ = '+';
            rem--;
        } else if (rem > 3) {
            *dst++ = '%';
            *dst++ = hex[c >> 4];
            *dst++ = hex[c & 0xF];
            rem -= 3;
        }
    }
    if (rem > 0) *dst = '\0';
    return dst;
}

/* HTML-escape src into chunked response output */
static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[64];
    int pos = 0;
    while (*s) {
        const char *ent = NULL;
        if      (*s == '<') ent = "&lt;";
        else if (*s == '>') ent = "&gt;";
        else if (*s == '&') ent = "&amp;";
        else if (*s == '"') ent = "&quot;";
        if (ent) {
            if (pos) { buf[pos] = '\0'; httpd_resp_sendstr_chunk(req, buf); pos = 0; }
            httpd_resp_sendstr_chunk(req, ent);
        } else {
            buf[pos++] = *s;
            if (pos >= (int)sizeof(buf) - 1) {
                buf[pos] = '\0';
                httpd_resp_sendstr_chunk(req, buf);
                pos = 0;
            }
        }
        s++;
    }
    if (pos) { buf[pos] = '\0'; httpd_resp_sendstr_chunk(req, buf); }
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

/* Two-level nested JSON extractor: finds "outer":{...} then extracts key inside. */
static bool json_extract_nested(const char *json, const char *outer,
                                 const char *key, char *out, size_t out_sz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", outer);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '{' && *p != '[') {
        /* value may be a string — try direct extract */
        return json_extract_str(json, key, out, out_sz);
    }
    /* find matching close brace/bracket */
    char open = *p, close = (open == '{') ? '}' : ']';
    int depth = 1;
    const char *inner = p + 1;
    p++;
    while (*p && depth > 0) {
        if (*p == open)  depth++;
        if (*p == close) depth--;
        p++;
    }
    /* create temporary null-terminated substring */
    size_t inner_len = (size_t)(p - inner);
    char *tmp = malloc(inner_len + 1);
    if (!tmp) return false;
    memcpy(tmp, inner, inner_len);
    tmp[inner_len] = '\0';
    bool found = json_extract_str(tmp, key, out, out_sz);
    free(tmp);
    return found;
}

/* Extract trigger/id/params from a TRIGGERcmd Socket.IO "message" payload. */
static void extract_message_fields(const char *payload,
                                   char *trigger, size_t trigger_sz,
                                   char *run_id,  size_t run_id_sz,
                                   char *params,  size_t params_sz)
{
    trigger[0] = run_id[0] = params[0] = '\0';

    json_extract_str(payload, "trigger", trigger, trigger_sz);
    json_extract_str(payload, "id",      run_id,  run_id_sz);
    json_extract_str(payload, "params",  params,  params_sz);
    if (!params[0]) json_extract_str(payload, "param", params, params_sz);
    if (!params[0]) json_extract_str(payload, "value", params, params_sz);

    if (!trigger[0]) json_extract_nested(payload, "data", "trigger", trigger, trigger_sz);
    if (!trigger[0]) json_extract_nested(payload, "message", "trigger", trigger, trigger_sz);
    if (!run_id[0])  json_extract_nested(payload, "data", "id",      run_id,  run_id_sz);
    if (!params[0])  json_extract_nested(payload, "data", "params",  params,  params_sz);
}

/* ── Color parser ────────────────────────────────────────────────────────── */

static bool parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    while (*s == ' ') s++;

    static const struct { const char *name; uint8_t r, g, b; } named[] = {
        {"red",     255,   0,   0}, {"green",     0, 200,   0},
        {"blue",      0,   0, 255}, {"white",    255, 255, 255},
        {"black",     0,   0,   0}, {"yellow",   255, 255,   0},
        {"cyan",      0, 255, 255}, {"magenta",  255,   0, 255},
        {"orange",  255, 165,   0}, {"purple",   128,   0, 128},
        {"pink",    255, 105, 180}, {"gray",     128, 128, 128},
        {"grey",    128, 128, 128},
    };
    for (int i = 0; i < (int)(sizeof(named)/sizeof(named[0])); i++) {
        if (strcasecmp(s, named[i].name) == 0) {
            *r = named[i].r; *g = named[i].g; *b = named[i].b;
            return true;
        }
    }

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

/* ── HTTPS helpers ───────────────────────────────────────────────────────── */

/* Allocates *body_out (caller frees). Returns content length or -1 on error. */
static int https_get_simple(const char *url, char **body_out)
{
    *body_out = NULL;
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    int64_t cl = esp_http_client_fetch_headers(client);
    int max_body = MAX_BODY;
    if (cl > 0 && cl < max_body) max_body = (int)cl;

    char *buf = malloc(max_body + 1);
    if (!buf) { esp_http_client_close(client); esp_http_client_cleanup(client); return -1; }

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

/* Authenticated GET. Returns content length or -1. */
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
    int64_t cl = esp_http_client_fetch_headers(client);
    int max_body = MAX_BODY;
    if (cl > 0 && cl < max_body) max_body = (int)cl;

    char *buf = malloc(max_body + 1);
    if (!buf) { esp_http_client_close(client); esp_http_client_cleanup(client); return -1; }

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

/* Authenticated form POST.  Returns HTTP status or -1.  *resp_body is malloc'd. */
static int https_post_form(const char *url, const char *token,
                           const char *form_body, char **resp_body)
{
    if (resp_body) *resp_body = NULL;
    char bearer[HW_TOKEN_MAX + 10];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    int body_len = (int)strlen(form_body);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");

    if (esp_http_client_open(client, body_len) != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_write(client, form_body, body_len);

    int64_t cl = esp_http_client_fetch_headers(client);
    int max_body = MAX_BODY;
    if (cl > 0 && cl < max_body) max_body = (int)cl;

    if (resp_body) {
        char *buf = malloc(max_body + 1);
        if (buf) {
            int total = 0;
            while (total < max_body) {
                int n = esp_http_client_read(client, buf + total, max_body - total);
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

/* GET to a bookmark URL — no certificate validation, fire-and-forget. */
static void call_url(const char *url)
{
    if (!url || !url[0]) {
        ESP_LOGI(TAG, "button pressed — no bookmark URL configured");
        return;
    }

    ESP_LOGI(TAG, "calling bookmark URL: %s", url);
    atoms3_led_set(255, 255, 255);  /* white flash while calling */

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        /* Intentionally no crt_bundle_attach: skip TLS certificate validation
         * so the user can use short/self-signed URLs (requires CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY). */
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "bookmark: client init failed");
        atoms3_led_set(64, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "bookmark URL → HTTP %d", status);
    if (status >= 200 && status < 300) {
        atoms3_led_set(0, 64, 0);   /* brief green: success */
    } else {
        atoms3_led_set(64, 0, 0);   /* brief red: error */
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}

/* ── Core2 UDP discovery + remote trigger ────────────────────────────────── */

static bool discover_core2(void)
{
    if (s_core2_ip[0]) return true;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TCMD_DISCOVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    const char *msg = "TCMD_DISCOVER";
    sendto(sock, msg, strlen(msg), 0,
           (struct sockaddr *)&dest, sizeof(dest));

    char buf[64];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&src, &src_len);
    close(sock);

    if (n <= 0) return false;
    buf[n] = '\0';

    if (strncmp(buf, "TCMD_CORE2 ", 11) != 0) return false;

    strncpy(s_core2_ip, buf + 11, sizeof(s_core2_ip) - 1);
    s_core2_ip[sizeof(s_core2_ip) - 1] = '\0';
    ESP_LOGI(TAG, "discovered Core2 at %s", s_core2_ip);
    return true;
}

static bool trigger_core2(const char *cmd, const char *params)
{
    if (!cmd || !cmd[0]) return false;

    atoms3_led_set(0, 0, 255);

    /* ── Try local HTTP POST ─────────────────────────────────────────── */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!s_core2_ip[0]) {
            if (!discover_core2()) break;
        }

        char url[80];
        snprintf(url, sizeof(url), "http://%s/trigger", s_core2_ip);

        char body[384];
        snprintf(body, sizeof(body),
                 "{\"trigger\":\"%s\",\"params\":\"%s\"}",
                 cmd, params ? params : "");

        esp_http_client_config_t cfg = {
            .url        = url,
            .method     = HTTP_METHOD_POST,
            .timeout_ms = 5000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) break;

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));

        esp_err_t err = esp_http_client_perform(client);
        int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
        esp_http_client_cleanup(client);

        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "Core2 local trigger '%s' -> HTTP %d", cmd, status);
            atoms3_led_set(0, 64, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            return true;
        }

        ESP_LOGW(TAG, "Core2 local trigger failed (%d), %s",
                 status, attempt == 0 ? "re-discovering..." : "trying cloud...");
        s_core2_ip[0] = '\0';
    }

    /* ── Cloud API fallback ──────────────────────────────────────────── */
    if (s_core2_name[0] && s_hw_token[0]) {
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"computer\":\"%s\",\"trigger\":\"%s\",\"params\":\"%s\"}",
                 s_core2_name, cmd, params ? params : "");

        char bearer[HW_TOKEN_MAX + 10];
        snprintf(bearer, sizeof(bearer), "Bearer %s", s_hw_token);

        esp_http_client_config_t cfg = {
            .url               = TCMD_BASE_URL "/api/run/triggerSave",
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = 10000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_header(client, "Authorization", bearer);
            esp_http_client_set_post_field(client, body, strlen(body));

            esp_err_t err = esp_http_client_perform(client);
            int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
            esp_http_client_cleanup(client);

            if (status >= 200 && status < 300) {
                ESP_LOGI(TAG, "Core2 cloud trigger '%s' -> HTTP %d", cmd, status);
                atoms3_led_set(0, 64, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                return true;
            }
            ESP_LOGE(TAG, "Core2 cloud trigger failed: HTTP %d", status);
        }
    }

    atoms3_led_set(64, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    return false;
}

/* ── Button polling ──────────────────────────────────────────────────────── */

typedef enum { BTN_NONE, BTN_SINGLE, BTN_DOUBLE, BTN_TRIPLE, BTN_LONG } btn_press_t;

/* Non-blocking poll called every ~20 ms.
 * BTN_LONG  — fires while the button is still held once LONG_PRESS_MS elapses.
 * BTN_SINGLE/DOUBLE/TRIPLE — fires after CLICK_WINDOW_MS of inactivity
 *   following the last release in a click sequence. */
static btn_press_t poll_button(void)
{
    static bool     s_pressed      = false;
    static uint32_t s_press_start  = 0;
    static bool     s_long_fired   = false;
    static int      s_click_count  = 0;
    static uint32_t s_window_start = 0;
    static bool     s_in_window    = false;

    bool currently_pressed = (gpio_get_level(BUTTON_GPIO) == 0);
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (currently_pressed && !s_pressed) {
        /* Falling edge */
        s_pressed     = true;
        s_long_fired  = false;
        s_press_start = now;
        s_in_window   = false;
    } else if (currently_pressed && s_pressed && !s_long_fired) {
        /* Still held — fire BTN_LONG once threshold is crossed */
        if (now - s_press_start >= LONG_PRESS_MS) {
            s_long_fired  = true;
            s_click_count = 0;
            s_in_window   = false;
            return BTN_LONG;
        }
    } else if (!currently_pressed && s_pressed) {
        /* Rising edge */
        s_pressed = false;
        if (!s_long_fired) {
            s_click_count++;
            s_window_start = now;
            s_in_window    = true;
        }
    } else if (s_in_window && (now - s_window_start >= CLICK_WINDOW_MS)) {
        /* Click window expired — emit the accumulated click count */
        s_in_window = false;
        int count   = s_click_count;
        s_click_count = 0;
        if (count == 1) return BTN_SINGLE;
        if (count == 2) return BTN_DOUBLE;
        return BTN_TRIPLE;  /* 3 or more collapses to triple */
    }
    return BTN_NONE;
}

/* ── HTTP config server ──────────────────────────────────────────────────── */

/* Shared page skeleton */
static const char s_html_head[] =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TRIGGERcmd AtomS3 Lite</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;"
         "min-height:100vh;display:flex;align-items:center;justify-content:center;"
         "padding:1.5rem}"
    ".card{background:#1e2130;border:1px solid #2d3148;border-radius:1rem;"
          "padding:2.5rem 2rem;max-width:420px;width:100%;"
          "box-shadow:0 8px 32px rgba(0,0,0,.4)}"
    "h1{font-size:1.4rem;color:#a5b4fc;margin:0 0 .5rem}"
    "h2{font-size:1.1rem;color:#a5b4fc;margin:1.5rem 0 .5rem}"
    "p,li{color:#94a3b8;font-size:.9rem;margin:0 0 .75rem}"
    "ol{color:#94a3b8;font-size:.9rem;margin:0 0 1rem;padding-left:1.4rem}"
    "ol li{margin-bottom:.35rem}"
    "strong{color:#e2e8f0}"
    ".code{font-family:monospace;font-size:2.5rem;font-weight:700;"
          "letter-spacing:.3em;color:#a5b4fc;text-align:center;"
          "background:#0f1117;border:2px solid #4f46e5;border-radius:.6rem;"
          "padding:.75rem 1.5rem;margin:1.25rem 0}"
    ".note{color:#64748b;font-size:.8rem;text-align:center;margin-bottom:1rem}"
    "hr{border:none;border-top:1px solid #2d3148;margin:1.5rem 0}"
    "label{display:block;color:#94a3b8;font-size:.85rem;margin:.5rem 0 .2rem}"
    "input{width:100%;box-sizing:border-box;padding:.5rem;background:#0f1117;"
          "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;"
          "font-size:.95rem;margin-bottom:.5rem}"
    "button{width:100%;padding:.7rem;background:#4f46e5;color:#fff;border:none;"
           "border-radius:.6rem;font-size:.95rem;font-weight:600;cursor:pointer;"
           "margin-top:.5rem}"
    "button:hover{background:#4338ca}"
    ".ok{color:#86efac;margin:.5rem 0;text-align:center;font-size:.9rem}"
    ".danger{background:#991b1b}"
    ".danger:hover{background:#7f1d1d}"
    "</style></head><body><div class='card'>";

static const char s_html_tail[] = "</div></body></html>";

static esp_err_t cfg_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    httpd_resp_sendstr_chunk(req, "<h1>TRIGGERcmd AtomS3 Lite</h1>");

    if (s_pair_code[0]) {
        /* ── Pairing mode ─────────────────────────────────────────────── */
        httpd_resp_sendstr_chunk(req,
            "<p>Your device is ready to pair with your TRIGGERcmd account:</p>"
            "<ol>"
            "<li>Go to <strong>www.triggercmd.com</strong> and sign in.</li>"
            "<li>Click your name in the upper right corner.</li>"
            "<li>Click <strong>Pair</strong>.</li>"
            "<li>Enter the pair code shown below.</li>"
            "</ol>");

        char code_block[32];
        snprintf(code_block, sizeof(code_block),
                 "<div class='code'>%s</div>", s_pair_code);
        httpd_resp_sendstr_chunk(req, code_block);
        httpd_resp_sendstr_chunk(req,
            "<p class='note'>Code expires in 10 minutes. "
            "A new code is fetched automatically.</p>");
    } else {
        /* ── Paired mode ──────────────────────────────────────────────── */
        httpd_resp_sendstr_chunk(req, "<p>Device is paired and connected.</p>");
    }

    /* ── Bookmark URL section ─────────────────────────────────────────── */
    httpd_resp_sendstr_chunk(req,
        "<hr>"
        "<h2>Short Bookmark URL</h2>"
        "<p>URL called (HTTP GET, no cert check) when the button is short-pressed.</p>"
        "<form method='POST' action='/bookmark'>"
        "<label>URL</label>"
        "<input type='url' name='url' value='");
    send_escaped(req, s_bookmark_url);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='https://example.com/...'>"
        "<button type='submit'>Save URL</button>"
        "</form>"
        "<hr>"
        "<h2>Double-Click Bookmark URL</h2>"
        "<p>URL called (HTTP GET, no cert check) on double-click.</p>"
        "<form method='POST' action='/bookmark3'>"
        "<label>URL</label>"
        "<input type='url' name='url' value='");
    send_escaped(req, s_bookmark_url3);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='https://example.com/...'>"
        "<button type='submit'>Save URL</button>"
        "</form>"
        "<hr>"
        "<h2>Triple-Click Bookmark URL</h2>"
        "<p>URL called (HTTP GET, no cert check) on triple-click.</p>"
        "<form method='POST' action='/bookmark4'>"
        "<label>URL</label>"
        "<input type='url' name='url' value='");
    send_escaped(req, s_bookmark_url4);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='https://example.com/...'>"
        "<button type='submit'>Save URL</button>"
        "</form>"
        "<hr>"
        "<h2>Long-Press Bookmark URL</h2>"
        "<p>URL called (HTTP GET, no cert check) when the button is long-pressed (&ge; 2 s).</p>"
        "<form method='POST' action='/bookmark2'>"
        "<label>URL</label>"
        "<input type='url' name='url' value='");
    send_escaped(req, s_bookmark_url2);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='https://example.com/...'>"
        "<button type='submit'>Save URL</button>"
        "</form>");

    /* ── Core2 remote trigger section ────────────────────────────────── */
    httpd_resp_sendstr_chunk(req,
        "<hr>"
        "<h2>Core2 Remote Trigger</h2>"
        "<p>Configure button presses to trigger commands on a Core2 device. "
        "Local HTTP is tried first; TRIGGERcmd cloud is the fallback. "
        "Leave a command blank to use the Bookmark URL instead.</p>"
        "<form method='POST' action='/core2'>"
        "<label>Core2 IP Address (leave blank for auto-discovery)</label>"
        "<input name='core2_ip' value='");
    send_escaped(req, s_core2_ip);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='192.168.1.42'>"
        "<label>Core2 Computer Name (for cloud fallback)</label>"
        "<input name='core2_name' value='");
    send_escaped(req, s_core2_name);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='TCMDCORE2-(MAC)'>"
        "<hr style='border-color:#1e2130'>"
        "<label>Single-Click Command</label>"
        "<input name='cmd1' value='");
    send_escaped(req, s_core2_cmd1);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='speak'>"
        "<label>Single-Click Params</label>"
        "<input name='par1' value='");
    send_escaped(req, s_core2_par1);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='Hello world'>"
        "<label>Double-Click Command</label>"
        "<input name='cmd2' value='");
    send_escaped(req, s_core2_cmd2);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='color'>"
        "<label>Double-Click Params</label>"
        "<input name='par2' value='");
    send_escaped(req, s_core2_par2);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='red'>"
        "<label>Triple-Click Command</label>"
        "<input name='cmd3' value='");
    send_escaped(req, s_core2_cmd3);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='play'>"
        "<label>Triple-Click Params</label>"
        "<input name='par3' value='");
    send_escaped(req, s_core2_par3);
    httpd_resp_sendstr_chunk(req,
        "'>"
        "<label>Long-Press Command</label>"
        "<input name='cmd4' value='");
    send_escaped(req, s_core2_cmd4);
    httpd_resp_sendstr_chunk(req,
        "' placeholder='sleep'>"
        "<label>Long-Press Params</label>"
        "<input name='par4' value='");
    send_escaped(req, s_core2_par4);
    httpd_resp_sendstr_chunk(req,
        "'>"
        "<button type='submit'>Save Core2 Settings</button>"
        "</form>");

    /* ── Secondary WiFi section ───────────────────────────────────────── */
    httpd_resp_sendstr_chunk(req,
        "<hr>"
        "<h2>Secondary Wi-Fi Networks</h2>"
        "<form method='POST' action='/wifi'>");

    char ssid2[33] = {0}, ssid3[33] = {0};
    wifi_get_ssid2(ssid2, sizeof(ssid2));
    wifi_get_ssid3(ssid3, sizeof(ssid3));

    httpd_resp_sendstr_chunk(req, "<label>SSID 2</label><input name='ssid2' value='");
    send_escaped(req, ssid2);
    httpd_resp_sendstr_chunk(req,
        "'>"
        "<label>Password 2</label><input type='password' name='pass2'>"
        "<label>SSID 3</label><input name='ssid3' value='");
    send_escaped(req, ssid3);
    httpd_resp_sendstr_chunk(req,
        "'>"
        "<label>Password 3</label><input type='password' name='pass3'>"
        "<button type='submit'>Save Networks</button>"
        "</form>");

    /* ── Re-provision section ─────────────────────────────────────────── */
    httpd_resp_sendstr_chunk(req,
        "<hr>"
        "<form method='POST' action='/reprovision'>"
        "<button type='submit' class='danger'>"
            "Re-provision (reassign to a different account)"
        "</button>"
        "</form>");

    httpd_resp_sendstr_chunk(req, s_html_tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t handle_bookmark_post(httpd_req_t *req, char *buf, size_t buf_sz,
                                       const char *nvs_key)
{
    int len = req->content_len;
    if (len <= 0 || len >= 600) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }
    char *body = calloc(len + 1, 1);
    if (!body) { httpd_resp_send_500(req); return ESP_OK; }
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n <= 0) { free(body); httpd_resp_send_500(req); return ESP_OK; }
        got += n;
    }
    body[got] = '\0';

    char url[BOOKMARK_MAX] = {0};
    form_get_field(body, "url", url, sizeof(url));
    free(body);

    strncpy(buf, url, buf_sz - 1);
    nvs_write_str(nvs_key, buf);
    ESP_LOGI(TAG, "%s saved: %s", nvs_key, buf);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t cfg_bookmark_handler(httpd_req_t *req)
    { return handle_bookmark_post(req, s_bookmark_url,  sizeof(s_bookmark_url),  NVS_KEY_BOOKMARK);  }
static esp_err_t cfg_bookmark2_handler(httpd_req_t *req)
    { return handle_bookmark_post(req, s_bookmark_url2, sizeof(s_bookmark_url2), NVS_KEY_BOOKMARK2); }
static esp_err_t cfg_bookmark3_handler(httpd_req_t *req)
    { return handle_bookmark_post(req, s_bookmark_url3, sizeof(s_bookmark_url3), NVS_KEY_BOOKMARK3); }
static esp_err_t cfg_bookmark4_handler(httpd_req_t *req)
    { return handle_bookmark_post(req, s_bookmark_url4, sizeof(s_bookmark_url4), NVS_KEY_BOOKMARK4); }

static esp_err_t cfg_wifi_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len >= 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }
    char *body = calloc(len + 1, 1);
    if (!body) { httpd_resp_send_500(req); return ESP_OK; }
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n <= 0) { free(body); httpd_resp_send_500(req); return ESP_OK; }
        got += n;
    }
    body[got] = '\0';

    char ssid2[33] = {0}, pass2[65] = {0};
    char ssid3[33] = {0}, pass3[65] = {0};
    form_get_field(body, "ssid2", ssid2, sizeof(ssid2));
    form_get_field(body, "pass2", pass2, sizeof(pass2));
    form_get_field(body, "ssid3", ssid3, sizeof(ssid3));
    form_get_field(body, "pass3", pass3, sizeof(pass3));
    free(body);

    /* Keep existing password if blank (but SSID was entered) */
    if (ssid2[0] && !pass2[0]) {
        nvs_handle_t h;
        if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
            size_t l = sizeof(pass2);
            nvs_get_str(h, "password2", pass2, &l);
            nvs_close(h);
        }
    }
    if (ssid3[0] && !pass3[0]) {
        nvs_handle_t h;
        if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
            size_t l = sizeof(pass3);
            nvs_get_str(h, "password3", pass3, &l);
            nvs_close(h);
        }
    }

    wifi_save_credentials2(ssid2, pass2);
    wifi_save_credentials3(ssid3, pass3);
    ESP_LOGI(TAG, "secondary WiFi updated (ssid2='%s' ssid3='%s')", ssid2, ssid3);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t cfg_reprovision_handler(httpd_req_t *req)
{
    nvs_erase_key_local(NVS_KEY_TOKEN);
    nvs_erase_key_local(NVS_KEY_COMPID);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    httpd_resp_sendstr_chunk(req,
        "<h1>TRIGGERcmd AtomS3 Lite</h1>"
        "<p class='ok'>&#10003; Re-provisioning &mdash; rebooting&hellip;</p>");
    httpd_resp_sendstr_chunk(req, s_html_tail);
    httpd_resp_sendstr_chunk(req, NULL);

    ESP_LOGI(TAG, "re-provision requested; rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t cfg_core2_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len >= 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }
    char *body = calloc(len + 1, 1);
    if (!body) { httpd_resp_send_500(req); return ESP_OK; }
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n <= 0) { free(body); httpd_resp_send_500(req); return ESP_OK; }
        got += n;
    }
    body[got] = '\0';

    char ip[CORE2_IP_MAX] = {0}, name[CORE2_NAME_MAX] = {0};
    char cmd1[CMD_NAME_MAX] = {0}, par1[CMD_PARAMS_MAX] = {0};
    char cmd2[CMD_NAME_MAX] = {0}, par2[CMD_PARAMS_MAX] = {0};
    char cmd3[CMD_NAME_MAX] = {0}, par3[CMD_PARAMS_MAX] = {0};
    char cmd4[CMD_NAME_MAX] = {0}, par4[CMD_PARAMS_MAX] = {0};

    form_get_field(body, "core2_ip",   ip,   sizeof(ip));
    form_get_field(body, "core2_name", name, sizeof(name));
    form_get_field(body, "cmd1",       cmd1, sizeof(cmd1));
    form_get_field(body, "par1",       par1, sizeof(par1));
    form_get_field(body, "cmd2",       cmd2, sizeof(cmd2));
    form_get_field(body, "par2",       par2, sizeof(par2));
    form_get_field(body, "cmd3",       cmd3, sizeof(cmd3));
    form_get_field(body, "par3",       par3, sizeof(par3));
    form_get_field(body, "cmd4",       cmd4, sizeof(cmd4));
    form_get_field(body, "par4",       par4, sizeof(par4));
    free(body);

    strncpy(s_core2_ip,   ip,   sizeof(s_core2_ip) - 1);
    strncpy(s_core2_name, name, sizeof(s_core2_name) - 1);
    strncpy(s_core2_cmd1, cmd1, sizeof(s_core2_cmd1) - 1);
    strncpy(s_core2_par1, par1, sizeof(s_core2_par1) - 1);
    strncpy(s_core2_cmd2, cmd2, sizeof(s_core2_cmd2) - 1);
    strncpy(s_core2_par2, par2, sizeof(s_core2_par2) - 1);
    strncpy(s_core2_cmd3, cmd3, sizeof(s_core2_cmd3) - 1);
    strncpy(s_core2_par3, par3, sizeof(s_core2_par3) - 1);
    strncpy(s_core2_cmd4, cmd4, sizeof(s_core2_cmd4) - 1);
    strncpy(s_core2_par4, par4, sizeof(s_core2_par4) - 1);

    nvs_write_str(NVS_KEY_CORE2_IP,   s_core2_ip);
    nvs_write_str(NVS_KEY_CORE2_NAME, s_core2_name);
    nvs_write_str(NVS_KEY_CMD1,       s_core2_cmd1);
    nvs_write_str(NVS_KEY_PAR1,       s_core2_par1);
    nvs_write_str(NVS_KEY_CMD2,       s_core2_cmd2);
    nvs_write_str(NVS_KEY_PAR2,       s_core2_par2);
    nvs_write_str(NVS_KEY_CMD3,       s_core2_cmd3);
    nvs_write_str(NVS_KEY_PAR3,       s_core2_par3);
    nvs_write_str(NVS_KEY_CMD4,       s_core2_cmd4);
    nvs_write_str(NVS_KEY_PAR4,       s_core2_par4);

    ESP_LOGI(TAG, "Core2 config saved: ip='%s' name='%s' cmd1='%s'",
             s_core2_ip, s_core2_name, s_core2_cmd1);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_config_server(void)
{
    if (s_cfg_server) return;

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 4;
    cfg.stack_size       = 8192;
    cfg.max_uri_handlers = 9;

    if (httpd_start(&s_cfg_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_cfg_server = NULL;
        return;
    }

    static const httpd_uri_t u_get   = { "/",            HTTP_GET,  cfg_get_handler,           NULL };
    static const httpd_uri_t u_bkmk  = { "/bookmark",    HTTP_POST, cfg_bookmark_handler,      NULL };
    static const httpd_uri_t u_bkmk2 = { "/bookmark2",   HTTP_POST, cfg_bookmark2_handler,     NULL };
    static const httpd_uri_t u_bkmk3 = { "/bookmark3",   HTTP_POST, cfg_bookmark3_handler,     NULL };
    static const httpd_uri_t u_bkmk4 = { "/bookmark4",   HTTP_POST, cfg_bookmark4_handler,     NULL };
    static const httpd_uri_t u_wifi  = { "/wifi",        HTTP_POST, cfg_wifi_handler,          NULL };
    static const httpd_uri_t u_repr  = { "/reprovision", HTTP_POST, cfg_reprovision_handler,   NULL };
    static const httpd_uri_t u_core2 = { "/core2",       HTTP_POST, cfg_core2_handler,         NULL };

    httpd_register_uri_handler(s_cfg_server, &u_get);
    httpd_register_uri_handler(s_cfg_server, &u_bkmk);
    httpd_register_uri_handler(s_cfg_server, &u_bkmk2);
    httpd_register_uri_handler(s_cfg_server, &u_bkmk3);
    httpd_register_uri_handler(s_cfg_server, &u_bkmk4);
    httpd_register_uri_handler(s_cfg_server, &u_wifi);
    httpd_register_uri_handler(s_cfg_server, &u_repr);
    httpd_register_uri_handler(s_cfg_server, &u_core2);

    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "Config server: http://" IPSTR "/", IP2STR(&ip.ip));
    }
}

/* ── SoftAP provisioning ─────────────────────────────────────────────────── */

static const char s_prov_html[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TRIGGERcmd AtomS3 Lite Setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh}"
    ".card{background:#1e2130;border:1px solid #2d3148;border-radius:1rem;"
    "padding:2rem;max-width:400px;width:100%}"
    "h1{color:#a5b4fc;font-size:1.3rem;margin:0 0 1rem}"
    "label{display:block;color:#94a3b8;font-size:.85rem;margin:.75rem 0 .2rem}"
    "input{width:100%;box-sizing:border-box;padding:.55rem;background:#0f1117;"
    "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:1rem}"
    "hr{border:none;border-top:1px solid #2d3148;margin:1.25rem 0 .5rem}"
    "button{margin-top:1.25rem;width:100%;padding:.75rem;background:#4f46e5;"
    "color:#fff;border:none;border-radius:.6rem;font-size:1rem;cursor:pointer}"
    "button:hover{background:#4338ca}"
    "</style></head><body><div class='card'>"
    "<h1>TRIGGERcmd AtomS3 Lite Setup</h1>"
    "<form method='POST' action='/save'>"
    "<label>Wi-Fi SSID</label><input name='ssid' required>"
    "<label>Wi-Fi Password</label><input name='pass' type='password'>"
    "<hr>"
    "<label>SSID 2 (optional)</label><input name='ssid2'>"
    "<label>Password 2</label><input name='pass2' type='password'>"
    "<label>SSID 3 (optional)</label><input name='ssid3'>"
    "<label>Password 3</label><input name='pass3' type='password'>"
    "<hr>"
    "<label>Single-Click Bookmark URL</label>"
    "<input name='bookmark' type='url' placeholder='https://...'>"
    "<p style='color:#64748b;font-size:.8rem;margin:.25rem 0 0'>"
    "Called on single click.</p>"
    "<label>Double-Click Bookmark URL</label>"
    "<input name='bookmark3' type='url' placeholder='https://...'>"
    "<p style='color:#64748b;font-size:.8rem;margin:.25rem 0 0'>"
    "Called on double click.</p>"
    "<label>Triple-Click Bookmark URL</label>"
    "<input name='bookmark4' type='url' placeholder='https://...'>"
    "<p style='color:#64748b;font-size:.8rem;margin:.25rem 0 0'>"
    "Called on triple click.</p>"
    "<label>Long-Press Bookmark URL</label>"
    "<input name='bookmark2' type='url' placeholder='https://...'>"
    "<p style='color:#64748b;font-size:.8rem;margin:.25rem 0 0'>"
    "Called on long press (&ge; 2 s).</p>"
    "<hr>"
    "<p style='color:#94a3b8;font-size:.85rem;margin:.5rem 0'>"
    "Core2 Remote Trigger (optional) &mdash; trigger commands on a Core2 "
    "picture frame. Local HTTP is tried first; cloud is the fallback.</p>"
    "<label>Core2 IP (blank = auto-discover)</label>"
    "<input name='core2_ip' placeholder='192.168.1.42'>"
    "<label>Core2 Computer Name (cloud fallback)</label>"
    "<input name='core2_name' placeholder='TCMDCORE2-(MAC)'>"
    "<hr style='border-color:#1e2130'>"
    "<label>Single-Click Command &amp; Params</label>"
    "<input name='cmd1' placeholder='speak'>"
    "<input name='par1' placeholder='Hello world'>"
    "<label>Double-Click Command &amp; Params</label>"
    "<input name='cmd2' placeholder='color'>"
    "<input name='par2' placeholder='red'>"
    "<label>Triple-Click Command &amp; Params</label>"
    "<input name='cmd3' placeholder='play'>"
    "<input name='par3'>"
    "<label>Long-Press Command &amp; Params</label>"
    "<input name='cmd4' placeholder='sleep'>"
    "<input name='par4'>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form></div></body></html>";

static const char s_prov_saved_html[] =
    "<!DOCTYPE html><html><body style='font-family:system-ui;background:#0f1117;"
    "color:#86efac;display:flex;align-items:center;justify-content:center;"
    "min-height:100vh'><h2>Saved! Device is restarting&hellip;</h2></body></html>";

static bool s_prov_done = false;

static esp_err_t prov_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_prov_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t prov_save_handler(httpd_req_t *req)
{
    char *body = malloc(4096);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    int  len = httpd_req_recv(req, body, 4095);
    if (len <= 0) { free(body); httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = '\0';

    char ssid[64]  = {0}, pass[64]  = {0};
    char ssid2[64] = {0}, pass2[64] = {0};
    char ssid3[64] = {0}, pass3[64] = {0};
    char bookmark[BOOKMARK_MAX]  = {0};
    char bookmark2[BOOKMARK_MAX] = {0};
    char bookmark3[BOOKMARK_MAX] = {0};
    char bookmark4[BOOKMARK_MAX] = {0};
    char ip[CORE2_IP_MAX]      = {0}, name[CORE2_NAME_MAX] = {0};
    char cmd1[CMD_NAME_MAX]    = {0}, par1[CMD_PARAMS_MAX] = {0};
    char cmd2[CMD_NAME_MAX]    = {0}, par2[CMD_PARAMS_MAX] = {0};
    char cmd3[CMD_NAME_MAX]    = {0}, par3[CMD_PARAMS_MAX] = {0};
    char cmd4[CMD_NAME_MAX]    = {0}, par4[CMD_PARAMS_MAX] = {0};

    form_get_field(body, "ssid",      ssid,      sizeof(ssid));
    form_get_field(body, "pass",      pass,      sizeof(pass));
    form_get_field(body, "ssid2",     ssid2,     sizeof(ssid2));
    form_get_field(body, "pass2",     pass2,     sizeof(pass2));
    form_get_field(body, "ssid3",     ssid3,     sizeof(ssid3));
    form_get_field(body, "pass3",     pass3,     sizeof(pass3));
    form_get_field(body, "bookmark",  bookmark,  sizeof(bookmark));
    form_get_field(body, "bookmark2", bookmark2, sizeof(bookmark2));
    form_get_field(body, "bookmark3", bookmark3, sizeof(bookmark3));
    form_get_field(body, "bookmark4", bookmark4, sizeof(bookmark4));
    form_get_field(body, "core2_ip",   ip,   sizeof(ip));
    form_get_field(body, "core2_name", name, sizeof(name));
    form_get_field(body, "cmd1", cmd1, sizeof(cmd1));
    form_get_field(body, "par1", par1, sizeof(par1));
    form_get_field(body, "cmd2", cmd2, sizeof(cmd2));
    form_get_field(body, "par2", par2, sizeof(par2));
    form_get_field(body, "cmd3", cmd3, sizeof(cmd3));
    form_get_field(body, "par3", par3, sizeof(par3));
    form_get_field(body, "cmd4", cmd4, sizeof(cmd4));
    form_get_field(body, "par4", par4, sizeof(par4));

    free(body);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_prov_saved_html, HTTPD_RESP_USE_STRLEN);

    if (ssid[0]) wifi_save_credentials(ssid, pass);
    wifi_save_credentials2(ssid2, pass2);
    wifi_save_credentials3(ssid3, pass3);
    if (bookmark[0])  nvs_write_str(NVS_KEY_BOOKMARK,  bookmark);
    if (bookmark2[0]) nvs_write_str(NVS_KEY_BOOKMARK2, bookmark2);
    if (bookmark3[0]) nvs_write_str(NVS_KEY_BOOKMARK3, bookmark3);
    if (bookmark4[0]) nvs_write_str(NVS_KEY_BOOKMARK4, bookmark4);
    if (ip[0])   nvs_write_str(NVS_KEY_CORE2_IP,   ip);
    if (name[0]) nvs_write_str(NVS_KEY_CORE2_NAME, name);
    if (cmd1[0]) nvs_write_str(NVS_KEY_CMD1, cmd1);
    if (par1[0]) nvs_write_str(NVS_KEY_PAR1, par1);
    if (cmd2[0]) nvs_write_str(NVS_KEY_CMD2, cmd2);
    if (par2[0]) nvs_write_str(NVS_KEY_PAR2, par2);
    if (cmd3[0]) nvs_write_str(NVS_KEY_CMD3, cmd3);
    if (par3[0]) nvs_write_str(NVS_KEY_PAR3, par3);
    if (cmd4[0]) nvs_write_str(NVS_KEY_CMD4, cmd4);
    if (par4[0]) nvs_write_str(NVS_KEY_PAR4, par4);

    s_prov_done = true;
    return ESP_OK;
}

static void run_softap_provisioning(void)
{
    wifi_stack_init_public();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "TCMD-AtomS3-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Starting SoftAP: %s", ap_ssid);

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

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.stack_size       = 12288;
    http_cfg.max_open_sockets = 4;
    http_cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    httpd_uri_t get_uri  = { .uri = "/",     .method = HTTP_GET,  .handler = prov_get_handler  };
    httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = prov_save_handler };
    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &save_uri);

    ESP_LOGI(TAG, "Connect to '%s', browse to 192.168.4.1", ap_ssid);

    bool led_on = false;
    s_prov_done = false;
    while (!s_prov_done) {
        led_on = !led_on;
        if (led_on) atoms3_led_set(0, 0, 48);
        else        atoms3_led_off();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    httpd_stop(server);
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── TRIGGERcmd provisioning helpers ─────────────────────────────────────── */

static void run_pair_code_flow(void)
{
    while (true) {
        char pair_url[192];
        snprintf(pair_url, sizeof(pair_url),
                 "%s/pair?model=%s", TCMD_BASE_URL, COMPUTER_MODEL);

        char *body = NULL;
        if (https_get_simple(pair_url, &body) < 0 || !body) {
            ESP_LOGW(TAG, "pair: failed to fetch code — retrying in 10 s");
            atoms3_led_set(64, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        char pair_code[8]    = {0};
        char pair_token[768] = {0};
        json_extract_str(body, "pairCode",  pair_code,  sizeof(pair_code));
        json_extract_str(body, "pairToken", pair_token, sizeof(pair_token));
        free(body);

        if (!pair_code[0] || !pair_token[0]) {
            ESP_LOGW(TAG, "pair: bad response — retrying in 10 s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        ESP_LOGI(TAG, "pair code: %s", pair_code);

        /* Update HTTP server to show the pair code */
        strncpy(s_pair_code, pair_code, sizeof(s_pair_code) - 1);

        /* White: waiting for user to authorize */
        atoms3_led_set(32, 32, 32);

        char lookup_url[900];
        snprintf(lookup_url, sizeof(lookup_url),
                 "%s/pair/lookup?token=%s", TCMD_BASE_URL, pair_token);

        bool paired = false;
        for (int i = 0; i < PAIR_MAX_POLLS && !paired; i++) {
            vTaskDelay(pdMS_TO_TICKS(PAIR_POLL_MS));

            char *lk = NULL;
            if (https_get_simple(lookup_url, &lk) >= 0 && lk) {
                char token[HW_TOKEN_MAX] = {0};
                if (json_extract_str(lk, "token", token, sizeof(token)) && token[0]) {
                    nvs_write_str(NVS_KEY_TOKEN, token);
                    strncpy(s_hw_token, token, sizeof(s_hw_token) - 1);
                    ESP_LOGI(TAG, "paired — token saved, rebooting");
                    paired = true;
                }
                free(lk);
            }
        }

        s_pair_code[0] = '\0';   /* clear pair code from HTTP page */

        if (paired) {
            atoms3_led_set(0, 64, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        ESP_LOGI(TAG, "pair code timed out — fetching new code");
    }
}

static void provision_computer(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char computer_name[COMPUTER_NAME_MAX];
    snprintf(computer_name, sizeof(computer_name),
             "%s-%02X%02X%02X%02X%02X%02X",
             COMPUTER_MODEL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "creating computer: %s", computer_name);

    char save_url[192];
    snprintf(save_url, sizeof(save_url), "%s/api/computer/save", TCMD_BASE_URL);

    char form[64];
    snprintf(form, sizeof(form), "name=%s&voice=atom+S+3", computer_name);

    char *resp = NULL;
    int status = https_post_form(save_url, s_hw_token, form, &resp);

    if (status >= 200 && status < 300 && resp) {
        char cid[COMPUTER_ID_MAX] = {0};
        if (json_extract_nested(resp, "data", "id", cid, sizeof(cid)) && cid[0]) {
            strncpy(s_computer_id, cid, sizeof(s_computer_id) - 1);
            nvs_write_str(NVS_KEY_COMPID, s_computer_id);
            ESP_LOGI(TAG, "computer_id saved: %s", s_computer_id);
        } else {
            ESP_LOGE(TAG, "computer/save: can't parse data.id; rebooting in 10 s");
            if (resp) free(resp);
            vTaskDelay(pdMS_TO_TICKS(10000));
            esp_restart();
        }
    } else {
        ESP_LOGE(TAG, "computer/save → HTTP %d; rebooting in 10 s", status);
        if (resp) free(resp);
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
    if (resp) free(resp);
}

/* ── Command sync ────────────────────────────────────────────────────────── */

typedef struct {
    const char *trigger;
    const char *allow_params;
    const char *mcp_desc;
    const char *icon;
} asl_cmd_t;

static const asl_cmd_t s_asl_cmds[] = {
    {
        "color", "true",
        "Change the LED color. Example: 'red' or '#FF0000'",
        "\xF0\x9F\x92\xA1" /* 💡 */
    },
    {
        "off", "false",
        "Turn the LED off.",
        "\xF0\x9F\x94\x8C" /* 🔌 */
    },
    {
        "reboot", "false",
        "Reboot the device.",
        "\xF0\x9F\x94\x81" /* 🔁 */
    },
};
#define ASL_CMD_COUNT (sizeof(s_asl_cmds) / sizeof(s_asl_cmds[0]))

static bool command_exists_online(const char *list_body, int list_len,
                                   const char *trigger)
{
    if (!list_body || list_len <= 0 || !trigger || !trigger[0]) return false;
    const char *p = list_body;
    while ((p = strstr(p, "\"")) != NULL) {
        const char *q = p + 1;
        bool is_name    = (strncmp(q, "name\"",    5) == 0);
        bool is_trigger = (strncmp(q, "trigger\"", 8) == 0);
        if (!is_name && !is_trigger) { p = q; continue; }
        q += is_name ? 5 : 8;
        while (*q == ' ' || *q == '\t' || *q == ':') q++;
        if (*q != '"') { p = q; continue; }
        q++;
        char name[64] = {0};
        size_t i = 0;
        bool esc = false;
        while (*q && (esc || *q != '"')) {
            if (!esc && *q == '\\') { esc = true; q++; continue; }
            if (i < sizeof(name) - 1) name[i++] = *q;
            esc = false; q++;
        }
        name[i] = '\0';
        if (strcasecmp(name, trigger) == 0) return true;
        p = q;
    }
    return false;
}

static void sync_command_if_missing(const asl_cmd_t *cmd, const char *cmd_url,
                                     const char *list_body, int list_len)
{
    if (command_exists_online(list_body, list_len, cmd->trigger)) {
        ESP_LOGI(TAG, "cmd '%s' already online — skip", cmd->trigger);
        return;
    }

    char body[512];
    char *p = body;
    char *bend = body + sizeof(body);

#define APPEND(k, v) \
    p = url_encode_append(p, (size_t)(bend - p), (k)); \
    if (p < bend) *p++ = '='; \
    p = url_encode_append(p, (size_t)(bend - p), (v)); \
    if (p < bend) *p++ = '&';

    APPEND("name",               cmd->trigger)
    APPEND("computer",           s_computer_id)
    APPEND("voice",              cmd->trigger)
    APPEND("voiceReply",         "")
    APPEND("allowParams",        cmd->allow_params)
    APPEND("mcpToolDescription", cmd->mcp_desc ? cmd->mcp_desc : "")
    APPEND("icon",               cmd->icon ? cmd->icon : "")

#undef APPEND

    if (p > body && *(p - 1) == '&') p--;
    *p = '\0';

    int cs = https_post_form(cmd_url, s_hw_token, body, NULL);
    ESP_LOGI(TAG, "cmd/save '%s' → HTTP %d", cmd->trigger, cs);
}

static void sync_commands(void)
{
    if (!s_computer_id[0] || !s_hw_token[0]) return;

    char list_url[192];
    snprintf(list_url, sizeof(list_url),
             "%s/api/command/list?computer_id=%s&limit=200", TCMD_BASE_URL, s_computer_id);

    char *list_body = NULL;
    int list_len = https_get_auth(list_url, s_hw_token, &list_body);

    if (!list_body || list_len <= 0) {
        ESP_LOGW(TAG, "cmd sync: list fetch failed");
        if (list_body) free(list_body);
        return;
    }

    char cmd_url[192];
    snprintf(cmd_url, sizeof(cmd_url), "%s/api/command/save", TCMD_BASE_URL);

    for (size_t i = 0; i < ASL_CMD_COUNT; i++) {
        sync_command_if_missing(&s_asl_cmds[i], cmd_url, list_body, list_len);
    }

    free(list_body);
}

/* ── Socket.IO ───────────────────────────────────────────────────────────── */

static void asl_event_handler(const char *event_name,
                               const char *payload_json,
                               void       *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "sio event: name='%s' payload=%.160s", event_name, payload_json);

    if (strcmp(event_name, "message") != 0) return;

    static char s_trigger[64];
    static char s_id[32];
    static char s_params[256];

    extract_message_fields(payload_json,
                           s_trigger, sizeof(s_trigger),
                           s_id,      sizeof(s_id),
                           s_params,  sizeof(s_params));

    ESP_LOGI(TAG, "dispatch: trigger='%s' id='%s' params='%s'",
             s_trigger, s_id, s_params);

    if (strcmp(s_trigger, "color") == 0) {
        uint8_t r = 0, g = 0, b = 0;
        if (parse_color(s_params, &r, &g, &b)) {
            s_pending_r = r;
            s_pending_g = g;
            s_pending_b = b;
            s_pending_color = true;
        } else {
            ESP_LOGW(TAG, "color: unrecognised '%s'", s_params);
        }

    } else if (strcmp(s_trigger, "off") == 0) {
        s_pending_off = true;

    } else if (strcmp(s_trigger, "reboot") == 0) {
        s_pending_reboot = true;

    } else {
        ESP_LOGW(TAG, "unknown trigger '%s'", s_trigger);
    }

    /* Queue run/save acknowledgement — must be sent from main task, not here,
     * to avoid blocking the WebSocket internal task and causing ping timeouts. */
    if (s_id[0] && s_computer_id[0]) {
        strncpy(s_pending_run_id, s_id, sizeof(s_pending_run_id) - 1);
        s_pending_run_id[sizeof(s_pending_run_id) - 1] = '\0';
        s_pending_run = true;
    }
}

static esp_err_t connect_and_subscribe(void)
{
    char sio_url[256];
    if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        snprintf(sio_url, sizeof(sio_url),
                 "wss://%s/socket.io/?EIO=4&transport=websocket"
                 "&__sails_io_sdk_version=0.11.0",
                 TCMD_BASE_URL + 8);
    } else {
        snprintf(sio_url, sizeof(sio_url),
                 "ws://%s/socket.io/?EIO=4&transport=websocket"
                 "&__sails_io_sdk_version=0.11.0",
                 TCMD_BASE_URL + 7);
    }

    esp_err_t ret = socketio_connect(sio_url, s_hw_token, asl_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "socketio_connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char sub_path[192];
    snprintf(sub_path, sizeof(sub_path),
             "/api/computer/subscribeToFunRoom?roomName=%s"
             "&__sails_io_sdk_version=0.11.0",
             s_computer_id);
    socketio_send_vget(sub_path, s_hw_token);

    ESP_LOGI(TAG, "Socket.IO connected and subscribed");
    return ESP_OK;
}

/* ── Main entry point ────────────────────────────────────────────────────── */

void tcmd_atoms3_lite_run(void)
{
    /* ── Hardware init ────────────────────────────────────────────────────── */
    atoms3_led_init();
    atoms3_led_set(100, 80, 0);  /* yellow: booting */

    ESP_LOGI(TAG, "TRIGGERcmd AtomS3 Lite firmware v%s", g_firmware_version);

    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* ── WiFi ─────────────────────────────────────────────────────────────── */
    if (!wifi_has_stored_credentials()) {
        atoms3_led_set(0, 0, 48);  /* dim blue: need provisioning */
        run_softap_provisioning();  /* never returns — esp_restart() */
    }

    if (wifi_connect() != ESP_OK) {
        atoms3_led_set(64, 0, 0);
        ESP_LOGE(TAG, "WiFi connect failed — halting");
        vTaskSuspend(NULL);
    }

    atoms3_led_set(0, 60, 0);   /* green: connected */
    vTaskDelay(pdMS_TO_TICKS(800));
    atoms3_led_off();

    /* ── Config HTTP server ───────────────────────────────────────────────── */
    start_config_server();

    /* ── Read NVS state ───────────────────────────────────────────────────── */
    bool have_token   = nvs_read_str(NVS_KEY_TOKEN,  s_hw_token,    sizeof(s_hw_token));
    bool have_comp_id = nvs_read_str(NVS_KEY_COMPID, s_computer_id, sizeof(s_computer_id));
    nvs_read_str(NVS_KEY_BOOKMARK,  s_bookmark_url,  sizeof(s_bookmark_url));
    nvs_read_str(NVS_KEY_BOOKMARK2, s_bookmark_url2, sizeof(s_bookmark_url2));
    nvs_read_str(NVS_KEY_BOOKMARK3, s_bookmark_url3, sizeof(s_bookmark_url3));
    nvs_read_str(NVS_KEY_BOOKMARK4, s_bookmark_url4, sizeof(s_bookmark_url4));

    nvs_read_str(NVS_KEY_CORE2_IP,   s_core2_ip,   sizeof(s_core2_ip));
    nvs_read_str(NVS_KEY_CORE2_NAME, s_core2_name, sizeof(s_core2_name));
    nvs_read_str(NVS_KEY_CMD1,       s_core2_cmd1, sizeof(s_core2_cmd1));
    nvs_read_str(NVS_KEY_PAR1,       s_core2_par1, sizeof(s_core2_par1));
    nvs_read_str(NVS_KEY_CMD2,       s_core2_cmd2, sizeof(s_core2_cmd2));
    nvs_read_str(NVS_KEY_PAR2,       s_core2_par2, sizeof(s_core2_par2));
    nvs_read_str(NVS_KEY_CMD3,       s_core2_cmd3, sizeof(s_core2_cmd3));
    nvs_read_str(NVS_KEY_PAR3,       s_core2_par3, sizeof(s_core2_par3));
    nvs_read_str(NVS_KEY_CMD4,       s_core2_cmd4, sizeof(s_core2_cmd4));
    nvs_read_str(NVS_KEY_PAR4,       s_core2_par4, sizeof(s_core2_par4));

    /* Try to discover Core2 on the LAN if no manual IP is configured */
    if (!s_core2_ip[0]) discover_core2();

    /* ── Pair code flow ───────────────────────────────────────────────────── */
    if (!have_token) {
        /* Slow white pulse while waiting for pair */
        atoms3_led_set(16, 16, 16);
        run_pair_code_flow();   /* loops until paired; reboots on success */
    }

    /* ── Computer provisioning ────────────────────────────────────────────── */
    if (!have_comp_id) {
        provision_computer();
        if (!s_computer_id[0]) {
            ESP_LOGE(TAG, "provisioning failed — halting");
            atoms3_led_set(64, 0, 0);
            vTaskSuspend(NULL);
        }
    }

    /* ── Sync commands ────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "syncing commands...");
    sync_commands();

    /* ── Restore last LED color ───────────────────────────────────────────── */
    {
        uint8_t lr = 0, lg = 0, lb = 0;
        bool have_r = nvs_read_u8(NVS_KEY_LED_R, &lr);
        bool have_g = nvs_read_u8(NVS_KEY_LED_G, &lg);
        bool have_b = nvs_read_u8(NVS_KEY_LED_B, &lb);
        if (have_r && have_g && have_b && (lr || lg || lb)) {
            atoms3_led_set(lr, lg, lb);
        } else {
            atoms3_led_set(20, 0, 20);  /* dim purple: idle/ready */
        }
    }

    /* ── Main event loop ──────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "ready — single/double/triple click + long press mapped to bookmark URLs");

    TickType_t last_ping_tick = xTaskGetTickCount();

    while (true) {
        /* Reconnect Socket.IO if needed */
        if (!socketio_connected()) {
            atoms3_led_set(0, 0, 24);  /* dim blue: connecting */
            esp_err_t ret = connect_and_subscribe();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "connection failed — retry in 10 s");
                vTaskDelay(pdMS_TO_TICKS(10000));
                socketio_disconnect();
                continue;
            }
            /* Restore LED after reconnect */
            uint8_t lr = 0, lg = 0, lb = 0;
            nvs_read_u8(NVS_KEY_LED_R, &lr);
            nvs_read_u8(NVS_KEY_LED_G, &lg);
            nvs_read_u8(NVS_KEY_LED_B, &lb);
            if (lr || lg || lb) atoms3_led_set(lr, lg, lb);
            else                atoms3_led_set(20, 0, 20);
            last_ping_tick = xTaskGetTickCount();
        }

        /* Apply pending LED color from Socket.IO */
        if (s_pending_color) {
            s_pending_color = false;
            atoms3_led_set(s_pending_r, s_pending_g, s_pending_b);
            nvs_write_u8(NVS_KEY_LED_R, s_pending_r);
            nvs_write_u8(NVS_KEY_LED_G, s_pending_g);
            nvs_write_u8(NVS_KEY_LED_B, s_pending_b);
        }

        if (s_pending_off) {
            s_pending_off = false;
            atoms3_led_off();
            nvs_write_u8(NVS_KEY_LED_R, 0);
            nvs_write_u8(NVS_KEY_LED_G, 0);
            nvs_write_u8(NVS_KEY_LED_B, 0);
        }

        if (s_pending_reboot) {
            s_pending_reboot = false;
            ESP_LOGW(TAG, "reboot command received");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }

        /* Send run/save acknowledgement */
        if (s_pending_run) {
            s_pending_run = false;
            char data_json[256];
            snprintf(data_json, sizeof(data_json),
                     "{\"status\":\"Command ran\","
                     "\"computer\":\"%s\","
                     "\"command\":\"%s\"}",
                     s_computer_id, s_pending_run_id);
            esp_err_t pe = socketio_send_vpost("/api/run/save",
                                               s_hw_token, data_json);
            ESP_LOGI(TAG, "run/save → %s", esp_err_to_name(pe));
        }

        /* Periodic EIO ping (every 20 s) */
        if ((xTaskGetTickCount() - last_ping_tick) >= pdMS_TO_TICKS(20000)) {
            socketio_send_eio_ping();
            last_ping_tick = xTaskGetTickCount();
        }

        /* Button handling */
        btn_press_t btn = poll_button();
        if (btn == BTN_SINGLE) {
            if (s_core2_cmd1[0]) trigger_core2(s_core2_cmd1, s_core2_par1);
            else call_url(s_bookmark_url);
        } else if (btn == BTN_DOUBLE) {
            if (s_core2_cmd2[0]) trigger_core2(s_core2_cmd2, s_core2_par2);
            else call_url(s_bookmark_url3);
        } else if (btn == BTN_TRIPLE) {
            if (s_core2_cmd3[0]) trigger_core2(s_core2_cmd3, s_core2_par3);
            else call_url(s_bookmark_url4);
        } else if (btn == BTN_LONG) {
            if (s_core2_cmd4[0]) trigger_core2(s_core2_cmd4, s_core2_par4);
            else call_url(s_bookmark_url2);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
