#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Connect using credentials stored in NVS, falling back to Kconfig defaults.
 * Blocks until an IP is obtained or the retry limit is reached.
 */
esp_err_t wifi_connect(void);

/**
 * Connect using the supplied SSID and password.
 * Safe to call multiple times – restarts the WiFi stack between attempts.
 */
esp_err_t wifi_connect_with_credentials(const char *ssid, const char *password);

/** Return true if a non-empty SSID is stored in NVS. */
bool wifi_has_stored_credentials(void);

/** Persist SSID and password to NVS. */
esp_err_t wifi_save_credentials(const char *ssid, const char *password);

/** Fill buf with the current IPv4 address string (e.g. "192.168.1.10"). */
esp_err_t wifi_get_ip(char *buf, size_t buf_len);
