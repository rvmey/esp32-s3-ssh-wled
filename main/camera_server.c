/*
 * ESP32-S3 camera variant — Seeed XIAO ESP32-S3 Sense (OV2640).
 *
 * Joins WiFi (Improv-WiFi provisioning over USB on first boot, stored
 * credentials thereafter), initialises the OV2640, and serves the latest
 * JPEG frame over HTTP:
 *
 *   GET /cam.jpg   → one fresh JPEG frame (Content-Type: image/jpeg)
 *   GET /          → a small status/help page
 *
 * The Core2 "camera" command polls /cam.jpg repeatedly to show a live view.
 * Frame size is QVGA (320x240) to match the Core2 panel and keep each frame
 * small for fast LAN transfer + software decode on the Core2.
 */

#include "camera_server.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_camera.h"

#include "wifi_manager.h"
#include "improv_wifi.h"

static const char *TAG = "camera";

/* ── Seeed XIAO ESP32-S3 Sense OV2640 pin map ────────────────────────────────
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
        "<img src=\"/cam.jpg\" style=\"max-width:100%\">";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    httpd_uri_t cam_uri = {
        .uri = "/cam.jpg", .method = HTTP_GET, .handler = cam_jpg_handler,
    };
    httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
    };
    httpd_register_uri_handler(server, &cam_uri);
    httpd_register_uri_handler(server, &root_uri);
    return ESP_OK;
}

void camera_server_run(void)
{
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

    char ip[16] = {0};
    if (wifi_get_ip(ip, sizeof(ip)) == ESP_OK) {
        ESP_LOGI(TAG, "Camera ready. On the Core2 run:  camera %s", ip);
        ESP_LOGI(TAG, "Preview: http://%s/cam.jpg", ip);
    } else {
        ESP_LOGI(TAG, "Camera ready (IP unavailable)");
    }

    /* HTTP server runs on its own task; idle here forever. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
