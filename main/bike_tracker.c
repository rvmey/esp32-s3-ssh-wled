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
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>

static const char *TAG = "bike_tracker";

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
    ESP_LOGI(TAG, "Entering deep sleep, INT wakeup on GPIO %d",
             CONFIG_MPU6050_INT_GPIO);

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

            gps_pvt_t pvt = {0};
            if (gps_ubx_get_pvt(&pvt) == ESP_OK && pvt.fix_type >= 2) {
                track_point_t pt = {
                    .lat       = pvt.lat,
                    .lon       = pvt.lon,
                    .speed_kmh = (int16_t)((pvt.speed_mm_s * 36 + 5000) / 10000),
                    ._pad      = 0,
                };
                ride_log_append(&pt);
                ESP_LOGD(TAG, "Point: lat=%ld lon=%ld spd=%d km/h×10",
                         (long)pt.lat, (long)pt.lon, pt.speed_kmh);
            }

            /* Check for inactivity via MPU6050 motion interrupt.           */
            if (mpu6050_is_active()) {
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

void bike_tracker_run(void)
{
    ride_log_init();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wakeup cause: %d", (int)cause);

    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        /* Woken by MPU6050 motion interrupt — start a tracking cycle.      */
        ESP_LOGI(TAG, "Motion detected — starting tracking cycle");
        run_tracking_cycle();
    } else {
        /* First power-on or manual reset.                                  */
        ESP_LOGI(TAG, "First boot / reset");

        /* If WiFi credentials are not stored, run Improv-WiFi provisioning
         * so the user can configure SSID, password and upload URL.         */
        if (!wifi_has_stored_credentials()) {
            ESP_LOGI(TAG, "No WiFi credentials — starting Improv-WiFi");
            if (improv_wifi_start() != ESP_OK) {
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
