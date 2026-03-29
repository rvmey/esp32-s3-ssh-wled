#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "wifi_manager.h"
#include "improv_wifi.h"
#include "led_control.h"
#include "ssh_server.h"

static const char *TAG = "main";

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

    /* Initialise the WS2812 RGB LED on GPIO 48 */
    led_init();

    if (!wifi_has_stored_credentials()) {
        /* No saved credentials – wait for the web installer to provision us */
        ESP_LOGI(TAG, "No WiFi credentials stored. "
                      "Open the installer page to configure WiFi.");
        led_set_color(0, 0, 64); /* blue = waiting for provisioning */
        if (improv_wifi_start() != ESP_OK) {
            led_set_color(64, 0, 0);
            ESP_LOGE(TAG, "Provisioning failed. Halting.");
            vTaskSuspend(NULL);
        }
    } else {
        /* Brief blue blink then connect with stored (or Kconfig) credentials */
        led_set_color(0, 0, 64);
        vTaskDelay(pdMS_TO_TICKS(300));
        led_off();

        if (wifi_connect() != ESP_OK) {
            led_set_color(64, 0, 0);
            ESP_LOGE(TAG, "Wi-Fi connection failed. Halting.");
            vTaskSuspend(NULL);
        }
    }

    /* Solid green → Wi-Fi connected */
    led_set_color(0, 32, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    led_off();

    /* Start the SSH server */
    if (ssh_server_start() != ESP_OK) {
        /* Solid red → SSH init failed */
        led_set_color(64, 0, 0);
        ESP_LOGE(TAG, "SSH server failed to start. Halting.");
        vTaskSuspend(NULL);
    }

    ESP_LOGI(TAG, "SSH server running. Connect with:");
    ESP_LOGI(TAG, "  ssh %s@<device-ip> -p %d",
             CONFIG_SSH_USERNAME, CONFIG_SSH_PORT);
}

