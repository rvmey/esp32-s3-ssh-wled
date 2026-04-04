#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

#include "wifi_manager.h"
#include "improv_wifi.h"
#include "ssh_server.h"
#include "http_config.h"

#if CONFIG_HARDWARE_DEVKITC
#include "led_control.h"
#elif CONFIG_HARDWARE_JC3248W535
#include "screen_control.h"
#elif CONFIG_HARDWARE_BIKE_TRACKER
#include "bike_tracker.h"
#endif

#define APP_VERSION "2.0.17"

static const char *TAG = "main";

/* Thin wrappers so startup code below is readable regardless of variant */
static inline void hw_init(void)
{
#if CONFIG_HARDWARE_DEVKITC
    led_init();
#elif CONFIG_HARDWARE_JC3248W535
    screen_init();
#elif CONFIG_HARDWARE_BIKE_TRACKER
    /* no-op: bike_tracker_run() manages its own peripherals */
#endif
}

static inline void hw_set_color(uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_HARDWARE_DEVKITC
    led_set_color(r, g, b);
#elif CONFIG_HARDWARE_JC3248W535
    screen_set_color(r, g, b);
#elif CONFIG_HARDWARE_BIKE_TRACKER
    (void)r; (void)g; (void)b;
#endif
}

static inline void hw_off(void)
{
#if CONFIG_HARDWARE_DEVKITC
    led_off();
#elif CONFIG_HARDWARE_JC3248W535
    screen_off();
#elif CONFIG_HARDWARE_BIKE_TRACKER
    /* no-op */
#endif
}

void app_main(void)
{
    /* NVS is required by Wi-Fi and for SSH host-key storage */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

#if CONFIG_HARDWARE_BIKE_TRACKER
    /* Bike tracker manages its own peripherals and sleep cycle.
     * This call never returns — it ends with esp_deep_sleep_start(). */
    bike_tracker_run();
    return;
#endif

    /* Initialise the output device for this hardware variant */
    hw_init();

    if (!wifi_has_stored_credentials()) {
        /* No saved credentials – wait for the web installer to provision us */
        ESP_LOGI(TAG, "No WiFi credentials stored. "
                      "Open the installer page to configure WiFi.");
        hw_set_color(0, 0, 64); /* blue = waiting for provisioning */
        if (improv_wifi_start() != ESP_OK) {
            hw_set_color(64, 0, 0);
            ESP_LOGE(TAG, "Provisioning failed. Halting.");
            vTaskSuspend(NULL);
        }
    } else {
        /* Brief blue blink then connect with stored (or Kconfig) credentials */
        hw_set_color(0, 0, 64);
        vTaskDelay(pdMS_TO_TICKS(300));
        hw_off();

        if (wifi_connect() != ESP_OK) {
            hw_set_color(64, 0, 0);
            ESP_LOGE(TAG, "Wi-Fi connection failed. Halting.");
            vTaskSuspend(NULL);
        }
    }

    /* Solid green → Wi-Fi connected */
    hw_set_color(0, 32, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    hw_off();

    /* Apply the saved initialization script now that WiFi status blinks are
     * done — color, text, landscape etc. will not be overwritten. */
    ssh_run_init_script();

    /* Start the HTTP config server so the user can set the SSH password */
    http_config_start();

    /* Start the SSH server */
    if (ssh_server_start() != ESP_OK) {
        hw_set_color(64, 0, 0);
        ESP_LOGE(TAG, "SSH server failed to start. Halting.");
        vTaskSuspend(NULL);
    }

    esp_rom_printf("esp32-s3-ssh-screen v" APP_VERSION "\n");
    ESP_LOGI(TAG, "esp32-s3-ssh-screen v" APP_VERSION);
    ESP_LOGI(TAG, "SSH server running. Connect with:");
    ESP_LOGI(TAG, "  ssh %s@<device-ip> -p %d",
             CONFIG_SSH_USERNAME, CONFIG_SSH_PORT);
}

