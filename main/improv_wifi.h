#pragma once

#include "esp_err.h"

/**
 * Run the Improv WiFi Serial v1 provisioning loop.
 *
 * Broadcasts the device's "Authorized" state over UART0 once per second so
 * that ESP Web Tools can detect it after flashing.  When the host sends a
 * "Send WiFi Settings" command the function:
 *   1. Attempts to connect with the supplied credentials.
 *   2. If successful: saves them to NVS and returns ESP_OK (already connected).
 *   3. If unsuccessful: sends an error response and keeps waiting.
 *
 * This function blocks until provisioning succeeds.
 */
esp_err_t improv_wifi_start(void);
