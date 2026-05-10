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
#include <limits.h>

static const char *TAG = "bike_tracker";

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

void bike_tracker_run(void)
{
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
