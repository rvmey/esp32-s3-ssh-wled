/*
 * ESP32-S3 camera variant — Seeed XIAO ESP32-S3 Sense (OV2640/OV3660,
 * auto-detected by the esp32-camera driver).
 *
 * Joins WiFi (Improv-WiFi provisioning over USB on first boot, stored
 * credentials thereafter), initialises the camera, and serves the latest
 * JPEG frame over HTTP:
 *
 *   GET /cam.jpg   → one fresh JPEG frame (Content-Type: image/jpeg)
 *   GET /          → a small status/help page
 *   GET /setup     → form to set the TRIGGERcmd token + Core2 computer name
 *   POST /setup    → saves those to NVS, then triggers the Core2 'camera' cmd
 *
 * Once the TRIGGERcmd token + Core2 computer name are configured (via /setup),
 * the camera POSTs to the TRIGGERcmd cloud at boot to run the Core2 'camera'
 * command with this camera's own IP, so the Core2 starts the live view by
 * itself.  The Core2 'camera' command polls /cam.jpg to show a live view.
 */

#include "camera_server.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_camera.h"
#include "nvs.h"

#include "wifi_manager.h"
#include "improv_wifi.h"

static const char *TAG = "camera";

/* ── Seeed XIAO ESP32-S3 Sense camera pin map ────────────────────────────────
 * Matches CAMERA_MODEL_XIAO_ESP32S3 from Espressif/Seeed reference camera_pins.h.
 * Verify against your board revision before flashing if init fails. */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40
#define CAM_PIN_SIOC    39
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

/* ── TRIGGERcmd remote-trigger config (persisted in NVS) ─────────────────── */
#define CAM_NVS_NS        "cam"
#define CAM_NVS_TOKEN     "tcmd_token"
#define CAM_NVS_COMPUTER  "core2_name"
#define TCMD_TOKEN_MAX    256
#define TCMD_COMPUTER_MAX 64

static char s_tcmd_token[TCMD_TOKEN_MAX]       = {0};
static char s_core2_name[TCMD_COMPUTER_MAX]    = {0};
static char s_cam_ip[16]                       = {0};

static void cam_cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CAM_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_tcmd_token);
    if (nvs_get_str(h, CAM_NVS_TOKEN, s_tcmd_token, &len) != ESP_OK) s_tcmd_token[0] = '\0';
    len = sizeof(s_core2_name);
    if (nvs_get_str(h, CAM_NVS_COMPUTER, s_core2_name, &len) != ESP_OK) s_core2_name[0] = '\0';
    nvs_close(h);
}

static void cam_cfg_save(const char *token, const char *computer)
{
    nvs_handle_t h;
    if (nvs_open(CAM_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, CAM_NVS_TOKEN, token);
    nvs_set_str(h, CAM_NVS_COMPUTER, computer);
    nvs_commit(h);
    nvs_close(h);
    strncpy(s_tcmd_token, token, sizeof(s_tcmd_token) - 1);
    s_tcmd_token[sizeof(s_tcmd_token) - 1] = '\0';
    strncpy(s_core2_name, computer, sizeof(s_core2_name) - 1);
    s_core2_name[sizeof(s_core2_name) - 1] = '\0';
}

/* POST {"computer","trigger":"camera","params":<ip>} to the TRIGGERcmd cloud so
 * the Core2 starts showing this camera's live view. */
static void cam_trigger_core2(void)
{
    if (!s_tcmd_token[0] || !s_core2_name[0] || !s_cam_ip[0]) {
        ESP_LOGI(TAG, "Skip Core2 trigger: set token + Core2 name at http://%s/setup",
                 s_cam_ip[0] ? s_cam_ip : "<ip>");
        return;
    }

    char body[160];
    snprintf(body, sizeof(body),
             "{\"computer\":\"%s\",\"trigger\":\"camera\",\"params\":\"%s\"}",
             s_core2_name, s_cam_ip);

    char bearer[TCMD_TOKEN_MAX + 8];
    snprintf(bearer, sizeof(bearer), "Bearer %s", s_tcmd_token);

    esp_http_client_config_t cfg = {
        .url = "https://www.triggercmd.com/api/run/triggerSave",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", bearer);
    esp_http_client_set_post_field(c, body, strlen(body));

    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Triggered Core2 '%s' camera %s -> HTTP %d",
                 s_core2_name, s_cam_ip, esp_http_client_get_status_code(c));
    } else {
        ESP_LOGE(TAG, "Core2 trigger failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
}

static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_QVGA,   /* 320x240, matches the Core2 panel */
        .jpeg_quality = 12,               /* 0-63, lower = better quality/larger */
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
    }
    return err;
}

/* GET /cam.jpg — return one fresh JPEG frame. */
static esp_err_t cam_jpg_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=cam.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    return res;
}

/* GET / — minimal status page. */
static esp_err_t root_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><meta charset=utf-8>"
        "<title>ESP32-S3 Camera</title>"
        "<h1>ESP32-S3 Camera</h1>"
        "<p>Live frame: <a href=\"/cam.jpg\">/cam.jpg</a></p>"
        "<p>TRIGGERcmd setup: <a href=\"/setup\">/setup</a></p>"
        "<img src=\"/cam.jpg\" style=\"max-width:100%\">";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

/* Decode an application/x-www-form-urlencoded value in place. */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* Copy the value of form field `key` from a urlencoded body into out (decoded). */
static void form_field(const char *body, const char *key, char *out, size_t out_sz)
{
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            if (n >= out_sz) n = out_sz - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static void send_setup_form(httpd_req_t *req, const char *banner)
{
    char ssid2[33] = {0};
    char ssid3[33] = {0};
    wifi_get_ssid2(ssid2, sizeof(ssid2));
    wifi_get_ssid3(ssid3, sizeof(ssid3));

    char html[2048];
    snprintf(html, sizeof(html),
        "<!doctype html><meta charset=utf-8><title>Camera setup</title>"
        "<h1>Camera setup</h1>%s"
        "<form method=POST action=/setup>"
        "<h2>TRIGGERcmd</h2>"
        "<p>API token:<br><input name=token size=50 value=\"%s\"></p>"
        "<p>Core2 computer name:<br><input name=computer size=40 value=\"%s\"></p>"
        "<h2>Extra WiFi networks</h2>"
        "<p>Network 2 SSID:<br><input name=ssid2 size=32 value=\"%.32s\"></p>"
        "<p>Network 2 password:<br><input name=pass2 type=password size=32></p>"
        "<p>Network 3 SSID:<br><input name=ssid3 size=32 value=\"%.32s\"></p>"
        "<p>Network 3 password:<br><input name=pass3 type=password size=32></p>"
        "<p><small>Leave password blank to keep existing. "
        "Clear SSID to remove that network.</small></p>"
        "<p><button>Save &amp; start Core2 view</button></p>"
        "</form>"
        "<p>On save, the Core2's <code>camera</code> command runs with this "
        "camera's IP (%s).</p>",
        banner ? banner : "",
        s_tcmd_token[0] ? "********" : "",
        s_core2_name,
        ssid2, ssid3,
        s_cam_ip);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

/* GET /setup — config form. */
static esp_err_t setup_get_handler(httpd_req_t *req)
{
    send_setup_form(req, NULL);
    return ESP_OK;
}

/* POST /setup — save token + Core2 name, then trigger the Core2 camera cmd. */
static esp_err_t setup_post_handler(httpd_req_t *req)
{
    char buf[512];
    int total = 0;
    int remaining = req->content_len < (int)sizeof(buf) - 1
                        ? req->content_len : (int)sizeof(buf) - 1;
    while (total < remaining) {
        int r = httpd_req_recv(req, buf + total, remaining - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';

    char token[TCMD_TOKEN_MAX], computer[TCMD_COMPUTER_MAX];
    char ssid2[33], pass2[65], ssid3[33], pass3[65];
    form_field(buf, "token",    token,    sizeof(token));
    form_field(buf, "computer", computer, sizeof(computer));
    form_field(buf, "ssid2",    ssid2,    sizeof(ssid2));
    form_field(buf, "pass2",    pass2,    sizeof(pass2));
    form_field(buf, "ssid3",    ssid3,    sizeof(ssid3));
    form_field(buf, "pass3",    pass3,    sizeof(pass3));

    /* Keep the existing token if the masked placeholder was left unchanged. */
    if (strcmp(token, "********") == 0) {
        strncpy(token, s_tcmd_token, sizeof(token) - 1);
        token[sizeof(token) - 1] = '\0';
    }

    cam_cfg_save(token, computer);
    ESP_LOGI(TAG, "Saved TRIGGERcmd config: computer='%s'", computer);

    /* Save extra WiFi networks.  If the SSID field is non-empty, save it.
     * If the SSID is empty, clear that slot.  If SSID is non-empty but
     * password is blank, preserve the existing stored password. */
    if (ssid2[0]) {
        if (pass2[0]) {
            wifi_save_credentials2(ssid2, pass2);
        } else {
            char existing_pass[65] = {0};
            nvs_handle_t h;
            if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
                size_t len = sizeof(existing_pass);
                nvs_get_str(h, "password2", existing_pass, &len);
                nvs_close(h);
            }
            wifi_save_credentials2(ssid2, existing_pass);
        }
        ESP_LOGI(TAG, "Saved WiFi network 2: '%s'", ssid2);
    } else {
        wifi_save_credentials2("", "");
        ESP_LOGI(TAG, "Cleared WiFi network 2");
    }
    if (ssid3[0]) {
        if (pass3[0]) {
            wifi_save_credentials3(ssid3, pass3);
        } else {
            char existing_pass[65] = {0};
            nvs_handle_t h;
            if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
                size_t len = sizeof(existing_pass);
                nvs_get_str(h, "password3", existing_pass, &len);
                nvs_close(h);
            }
            wifi_save_credentials3(ssid3, existing_pass);
        }
        ESP_LOGI(TAG, "Saved WiFi network 3: '%s'", ssid3);
    } else {
        wifi_save_credentials3("", "");
        ESP_LOGI(TAG, "Cleared WiFi network 3");
    }

    cam_trigger_core2();
    send_setup_form(req, "<p style=color:green>Saved. Core2 view started.</p>");
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    /* LWIP_MAX_SOCKETS is 8 (sdkconfig.defaults); the HTTP server reserves 3
     * internally, so max_open_sockets must be <= 5.  We only serve a couple of
     * concurrent clients, so cap it well under the limit (default 7 fails). */
    config.max_open_sockets = 4;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    httpd_uri_t uris[] = {
        { .uri = "/cam.jpg", .method = HTTP_GET,  .handler = cam_jpg_handler },
        { .uri = "/setup",   .method = HTTP_GET,  .handler = setup_get_handler },
        { .uri = "/setup",   .method = HTTP_POST, .handler = setup_post_handler },
        { .uri = "/",        .method = HTTP_GET,  .handler = root_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    return ESP_OK;
}

void camera_server_run(void)
{
    cam_cfg_load();

    /* WiFi: Improv provisioning on first boot, stored credentials afterwards. */
    if (!wifi_has_stored_credentials()) {
        ESP_LOGI(TAG, "No WiFi credentials stored. "
                      "Open the installer page to provision over USB.");
        if (improv_wifi_start() != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning failed. Halting.");
            vTaskSuspend(NULL);
        }
    } else if (wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection failed. Halting.");
        vTaskSuspend(NULL);
    }

    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed. Halting.");
        vTaskSuspend(NULL);
    }

    if (start_http_server() != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start. Halting.");
        vTaskSuspend(NULL);
    }

    if (wifi_get_ip(s_cam_ip, sizeof(s_cam_ip)) == ESP_OK) {
        ESP_LOGI(TAG, "Camera ready: http://%s/cam.jpg  (setup: http://%s/setup)",
                 s_cam_ip, s_cam_ip);
    } else {
        ESP_LOGW(TAG, "Camera ready but IP unavailable");
    }

    /* Auto-start the Core2 live view if TRIGGERcmd is configured. */
    cam_trigger_core2();

    /* HTTP server runs on its own task; idle here forever. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
