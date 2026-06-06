#include "bike_tracker.h"

#include "mpu6050.h"
#include "gps_ubx.h"
#include "ride_log.h"
#include "ride_upload.h"
#include "wifi_manager.h"
#include "improv_wifi.h"
#include "sdkconfig.h"

#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bike_tracker";

/* ── Post-provisioning WiFi settings window ──────────────────────────────── */

static bool s_wifi_settings_done = false;

static void bt_url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16); r += 3;
        } else if (*r == '+') { *w++ = ' '; r++;
        } else { *w++ = *r++; }
    }
    *w = '\0';
}

static void bt_form_get(const char *body, const char *key, char *out, size_t max)
{
    size_t klen = strlen(key);
    const char *p = body; out[0] = '\0';
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < max - 1) out[i++] = *p++;
            out[i] = '\0'; bt_url_decode(out); return;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
}

static esp_err_t bt_wifi_get_handler(httpd_req_t *req)
{
    char ssid2[33] = {0}, ssid3[33] = {0};
    wifi_get_ssid2(ssid2, sizeof(ssid2));
    wifi_get_ssid3(ssid3, sizeof(ssid3));

    const char *head =
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Bike Tracker Wi-Fi</title>"
        "<style>body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh}"
        ".card{background:#1e2130;border:1px solid #2d3148;border-radius:1rem;"
        "padding:2rem;max-width:380px;width:100%}"
        "h1{color:#a5b4fc;font-size:1.3rem;margin:0 0 .5rem}"
        "p{color:#94a3b8;font-size:.85rem;margin:0 0 1rem}"
        "label{display:block;color:#94a3b8;font-size:.85rem;margin:.6rem 0 .2rem}"
        "input{width:100%;box-sizing:border-box;padding:.6rem;background:#0f1117;"
        "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:.95rem}"
        "button{margin-top:1.25rem;width:100%;padding:.75rem;background:#4f46e5;"
        "color:#fff;border:none;border-radius:.6rem;font-size:1rem;cursor:pointer}"
        "</style></head><body><div class='card'>"
        "<h1>Secondary Wi-Fi Networks</h1>"
        "<p>Optional. Leave blank to skip.</p>"
        "<form method='POST' action='/wifi'>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, head);

    /* SSID2 input with current value */
    char buf[512];
    snprintf(buf, sizeof(buf),
             "<label>SSID 2</label><input name='ssid2' value='%s'>"
             "<label>Password 2</label><input name='pass2' type='password'>"
             "<label>SSID 3</label><input name='ssid3' value='%s'>"
             "<label>Password 3</label><input name='pass3' type='password'>",
             ssid2, ssid3);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req,
        "<button type='submit'>Save &amp; Continue</button>"
        "</form></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t bt_wifi_post_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len >= 512) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }
    char *body = calloc(len + 1, 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL); return ESP_OK; }
    int got = 0, rem = len;
    while (rem > 0) { int n = httpd_req_recv(req, body + got, rem); if (n <= 0) { free(body); return ESP_FAIL; } got += n; rem -= n; }

    char ssid2[33] = {0}, pass2[65] = {0};
    char ssid3[33] = {0}, pass3[65] = {0};
    bt_form_get(body, "ssid2", ssid2, sizeof(ssid2));
    bt_form_get(body, "pass2", pass2, sizeof(pass2));
    bt_form_get(body, "ssid3", ssid3, sizeof(ssid3));
    bt_form_get(body, "pass3", pass3, sizeof(pass3));
    free(body);

    if (ssid2[0] && !pass2[0]) {
        nvs_handle_t h;
        if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) { size_t l = sizeof(pass2); nvs_get_str(h, "password2", pass2, &l); nvs_close(h); }
    }
    if (ssid3[0] && !pass3[0]) {
        nvs_handle_t h;
        if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) { size_t l = sizeof(pass3); nvs_get_str(h, "password3", pass3, &l); nvs_close(h); }
    }

    wifi_save_credentials2(ssid2, pass2);
    wifi_save_credentials3(ssid3, pass3);
    ESP_LOGI(TAG, "Secondary WiFi saved: ssid2='%s' ssid3='%s'", ssid2, ssid3);

    const char *done =
        "<!DOCTYPE html><html><body style='font-family:system-ui;background:#0f1117;"
        "color:#86efac;display:flex;align-items:center;justify-content:center;"
        "min-height:100vh'><h2>Saved! Continuing&hellip;</h2></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, done, HTTPD_RESP_USE_STRLEN);
    s_wifi_settings_done = true;
    return ESP_OK;
}

/* Start a temporary HTTP server; block until the form is submitted or timeout_ms elapses. */
static void run_wifi_settings_window(int timeout_ms)
{
    s_wifi_settings_done = false;

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 3;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Could not start WiFi settings server");
        return;
    }

    static const httpd_uri_t u_get  = { "/",     HTTP_GET,  bt_wifi_get_handler,  NULL };
    static const httpd_uri_t u_post = { "/wifi",  HTTP_POST, bt_wifi_post_handler, NULL };
    httpd_register_uri_handler(server, &u_get);
    httpd_register_uri_handler(server, &u_post);

    char ip[24] = {0};
    if (wifi_get_ip(ip, sizeof(ip)) == ESP_OK)
        ESP_LOGI(TAG, "WiFi settings window open at http://%s (%.0f s)", ip, timeout_ms / 1000.0f);

    int elapsed = 0;
    while (!s_wifi_settings_done && elapsed < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(500));
        elapsed += 500;
    }

    httpd_stop(server);
    ESP_LOGI(TAG, "WiFi settings window closed%s",
             s_wifi_settings_done ? " (form submitted)" : " (timeout)");
}

#if CONFIG_TRACKER_HALL_ACTIVE_HIGH
#define HALL_INTR_TYPE GPIO_INTR_POSEDGE
#define HALL_PULLUP    GPIO_PULLUP_DISABLE
#define HALL_PULLDOWN  GPIO_PULLDOWN_ENABLE
#else
#define HALL_INTR_TYPE GPIO_INTR_NEGEDGE
#define HALL_PULLUP    GPIO_PULLUP_ENABLE
#define HALL_PULLDOWN  GPIO_PULLDOWN_DISABLE
#endif

#if CONFIG_TRACKER_HALL_ENABLE
static volatile uint32_t s_hall_pulse_count = 0;
static portMUX_TYPE s_hall_pulse_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR hall_isr_handler(void *arg)
{
    (void)arg;
    portENTER_CRITICAL_ISR(&s_hall_pulse_mux);
    s_hall_pulse_count++;
    portEXIT_CRITICAL_ISR(&s_hall_pulse_mux);
}

static uint32_t hall_take_pulses(void)
{
    uint32_t pulses;

    portENTER_CRITICAL(&s_hall_pulse_mux);
    pulses = s_hall_pulse_count;
    s_hall_pulse_count = 0;
    portEXIT_CRITICAL(&s_hall_pulse_mux);

    return pulses;
}

static int16_t hall_pulses_to_speed_kmh_x10(uint32_t pulses, int interval_ms)
{
    if (pulses == 0 || interval_ms <= 0) {
        return 0;
    }

    int64_t distance_mm = ((int64_t)pulses * CONFIG_TRACKER_WHEEL_CIRCUM_MM
                           + (CONFIG_TRACKER_HALL_PULSES_PER_REV / 2))
                          / CONFIG_TRACKER_HALL_PULSES_PER_REV;
    int64_t speed_mm_s = (distance_mm * 1000 + (interval_ms / 2)) / interval_ms;
    int64_t speed_kmh_x10 = (speed_mm_s * 36 + 500) / 1000;

    if (speed_kmh_x10 > INT16_MAX) {
        speed_kmh_x10 = INT16_MAX;
    }
    return (int16_t)speed_kmh_x10;
}

static esp_err_t hall_sensor_runtime_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_TRACKER_HALL_GPIO,
        .mode = GPIO_MODE_INPUT,
        .intr_type = HALL_INTR_TYPE,
        .pull_up_en = HALL_PULLUP,
        .pull_down_en = HALL_PULLDOWN,
    };

    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Hall GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Hall ISR service install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add((gpio_num_t)CONFIG_TRACKER_HALL_GPIO,
                               hall_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Hall ISR add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    hall_take_pulses();
    ESP_LOGI(TAG, "Hall speed sensor active on GPIO %d", CONFIG_TRACKER_HALL_GPIO);
    return ESP_OK;
}
#endif

/* ── Deep sleep helper ───────────────────────────────────────────────────── */

static void IRAM_ATTR enter_deep_sleep(void)
{
    /* Re-arm MPU6050 motion interrupt as wakeup source.                    */
    mpu6050_clear_interrupt();
    mpu6050_configure_wakeup();

    /* Enable internal RTC pull-down so a floating / disconnected INT pin
     * does not immediately re-trigger EXT0 and cause a boot loop.         */
    rtc_gpio_init((gpio_num_t)CONFIG_MPU6050_INT_GPIO);
    rtc_gpio_set_direction((gpio_num_t)CONFIG_MPU6050_INT_GPIO,
                           RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis((gpio_num_t)CONFIG_MPU6050_INT_GPIO);
    rtc_gpio_pulldown_en((gpio_num_t)CONFIG_MPU6050_INT_GPIO);

    esp_sleep_enable_ext0_wakeup((gpio_num_t)CONFIG_MPU6050_INT_GPIO, 1);

#if CONFIG_TRACKER_HALL_ENABLE
    rtc_gpio_init((gpio_num_t)CONFIG_TRACKER_HALL_GPIO);
    rtc_gpio_set_direction((gpio_num_t)CONFIG_TRACKER_HALL_GPIO,
                           RTC_GPIO_MODE_INPUT_ONLY);
#if CONFIG_TRACKER_HALL_ACTIVE_HIGH
    rtc_gpio_pullup_dis((gpio_num_t)CONFIG_TRACKER_HALL_GPIO);
    rtc_gpio_pulldown_en((gpio_num_t)CONFIG_TRACKER_HALL_GPIO);
    esp_sleep_enable_ext1_wakeup(1ULL << CONFIG_TRACKER_HALL_GPIO,
                                 ESP_EXT1_WAKEUP_ANY_HIGH);
    ESP_LOGI(TAG, "Entering deep sleep, wakeups: MPU INT GPIO %d high, hall GPIO %d high",
             CONFIG_MPU6050_INT_GPIO, CONFIG_TRACKER_HALL_GPIO);
#else
    rtc_gpio_pullup_en((gpio_num_t)CONFIG_TRACKER_HALL_GPIO);
    rtc_gpio_pulldown_dis((gpio_num_t)CONFIG_TRACKER_HALL_GPIO);
    esp_sleep_enable_ext1_wakeup(1ULL << CONFIG_TRACKER_HALL_GPIO,
                                 ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGI(TAG, "Entering deep sleep, wakeups: MPU INT GPIO %d high, hall GPIO %d low",
             CONFIG_MPU6050_INT_GPIO, CONFIG_TRACKER_HALL_GPIO);
#endif
#else
    ESP_LOGI(TAG, "Entering deep sleep, INT wakeup on GPIO %d",
             CONFIG_MPU6050_INT_GPIO);
#endif

    /* Log the hall pin level so boot-loop causes are visible in the log.  */
#if CONFIG_TRACKER_HALL_ENABLE
    ESP_LOGI(TAG, "Hall GPIO %d level before sleep: %d",
             CONFIG_TRACKER_HALL_GPIO,
             (int)rtc_gpio_get_level((gpio_num_t)CONFIG_TRACKER_HALL_GPIO));
#endif

    /* Short delay to let log output flush before power-down.               */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
    /* never reached */
}

/* ── Tracking cycle ──────────────────────────────────────────────────────── */

static void run_tracking_cycle(void)
{
    /* Re-initialise I2C and MPU6050 after deep-sleep reset.                */
    if (mpu6050_init() != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 init failed — going back to sleep");
        enter_deep_sleep();
    }
    mpu6050_clear_interrupt();

#if CONFIG_TRACKER_HALL_ENABLE
    if (hall_sensor_runtime_init() != ESP_OK) {
        ESP_LOGW(TAG, "Hall speed sensor init failed; continuing with GPS speed only");
    }
#endif

    /* Acquire GPS fix. */
    if (gps_ubx_init() != ESP_OK) {
        ESP_LOGE(TAG, "GPS init failed — going back to sleep");
        enter_deep_sleep();
    }

    bool ride_open = false;
    esp_err_t fix_ret = gps_ubx_wait_fix(CONFIG_GPS_FIX_TIMEOUT_S);
    if (fix_ret == ESP_OK) {
        gps_pvt_t pvt = {0};
        gps_ubx_get_pvt(&pvt);
        ride_log_start(pvt.unix_ts);
        ride_open = true;
        ESP_LOGI(TAG, "Ride started");
    } else {
        ESP_LOGW(TAG, "No GPS fix — skipping ride log");
    }

    /* ── TRACKING loop ──────────────────────────────────────────────────── */
    if (ride_open) {
        const int interval_ms  = CONFIG_TRACKER_LOG_INTERVAL_S * 1000;
        const int inact_limit  = CONFIG_TRACKER_INACTIVITY_TIMEOUT_S
                                 / CONFIG_TRACKER_LOG_INTERVAL_S;
        int inactivity_ticks   = 0;

        ESP_LOGI(TAG, "Tracking (interval=%ds, inactivity=%ds)",
                 CONFIG_TRACKER_LOG_INTERVAL_S,
                 CONFIG_TRACKER_INACTIVITY_TIMEOUT_S);

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));

#if CONFIG_TRACKER_HALL_ENABLE
            uint32_t hall_pulses = hall_take_pulses();
            int16_t hall_speed_kmh_x10 = hall_pulses_to_speed_kmh_x10(hall_pulses,
                                                                       interval_ms);
#else
            uint32_t hall_pulses = 0;
#endif

            gps_pvt_t pvt = {0};
            if (gps_ubx_get_pvt(&pvt) == ESP_OK && pvt.fix_type >= 2) {
                int16_t speed_kmh_x10 =
                    (int16_t)((pvt.speed_mm_s * 36 + 5000) / 10000);
#if CONFIG_TRACKER_HALL_ENABLE
                if (hall_pulses > 0) {
                    speed_kmh_x10 = hall_speed_kmh_x10;
                }
#endif
                track_point_t pt = {
                    .lat       = pvt.lat,
                    .lon       = pvt.lon,
                    .speed_kmh = speed_kmh_x10,
                    ._pad      = 0,
                };
                ride_log_append(&pt);
#if CONFIG_TRACKER_HALL_ENABLE
                ESP_LOGD(TAG, "Point: lat=%ld lon=%ld spd=%d km/hx10 hall_pulses=%" PRIu32,
                         (long)pt.lat, (long)pt.lon, pt.speed_kmh, hall_pulses);
#else
                ESP_LOGD(TAG, "Point: lat=%ld lon=%ld spd=%d km/hx10",
                         (long)pt.lat, (long)pt.lon, pt.speed_kmh);
#endif
            }

            /* Check for inactivity via MPU6050 interrupt or hall pulses.   */
            if (mpu6050_is_active() || hall_pulses > 0) {
                inactivity_ticks = 0;
            } else {
                inactivity_ticks++;
                ESP_LOGD(TAG, "Inactivity ticks: %d/%d",
                         inactivity_ticks, inact_limit);
            }

            if (inactivity_ticks >= inact_limit) {
                ESP_LOGI(TAG, "Inactivity timeout — stopping ride");
                break;
            }
        }

        ride_log_finish();
    }

    gps_ubx_deinit();

    /* ── STOPPING: upload if WiFi is available ──────────────────────────── */
    if (ride_open) {
        ESP_LOGI(TAG, "Connecting WiFi for upload ...");
        if (wifi_connect() == ESP_OK) {
            ride_upload_all();
            esp_wifi_stop();
        } else {
            ESP_LOGW(TAG, "WiFi failed — rides kept in NVS for next time");
        }
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

extern const char g_firmware_version[];

void bike_tracker_run(void)
{
    ESP_LOGI(TAG, "bike_tracker v%s", g_firmware_version);
    ride_log_init();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wakeup cause: %d", (int)cause);

    if (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1) {
        /* Woken by MPU6050 interrupt (EXT0) or hall pulse (EXT1).          */
        ESP_LOGI(TAG, "Motion/speed wakeup detected — starting tracking cycle");
        run_tracking_cycle();
    } else {
        /* First power-on or manual reset.                                  */
        ESP_LOGI(TAG, "First boot / reset");

        /* If WiFi credentials are not stored, run Improv-WiFi provisioning
         * so the user can configure SSID, password and upload URL.         */
        if (!wifi_has_stored_credentials()) {
            ESP_LOGI(TAG, "No WiFi credentials — starting Improv-WiFi");
            if (improv_wifi_start() == ESP_OK) {
                /* Give the user 60 s to optionally set secondary SSIDs. */
                run_wifi_settings_window(60000);
            } else {
                ESP_LOGW(TAG, "Improv-WiFi failed (will retry next boot)");
            }
        }

        /* Initialise MPU6050 and arm the wakeup interrupt.                 */
        if (mpu6050_init() != ESP_OK) {
            ESP_LOGE(TAG, "MPU6050 init failed on first boot");
            /* Continue to sleep — will retry on next power cycle.          */
        }
    }

    enter_deep_sleep();
}
