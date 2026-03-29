#pragma once

#include "esp_err.h"

/**
 * @brief Connect to the Wi-Fi network configured via Kconfig (menuconfig).
 *        Blocks until the device obtains an IP address or fails after the
 *        retry limit.
 *
 * @return ESP_OK on success, ESP_FAIL if the connection could not be
 *         established after all retries.
 */
esp_err_t wifi_connect(void);
