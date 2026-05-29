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

/** Persist primary SSID and password to NVS. */
esp_err_t wifi_save_credentials(const char *ssid, const char *password);

/** Persist secondary SSID and password to NVS (pass empty ssid to clear). */
esp_err_t wifi_save_credentials2(const char *ssid, const char *password);

/** Persist tertiary SSID and password to NVS (pass empty ssid to clear). */
esp_err_t wifi_save_credentials3(const char *ssid, const char *password);

/** Fill buf with the current IPv4 address string (e.g. "192.168.1.10"). */
esp_err_t wifi_get_ip(char *buf, size_t buf_len);

/**
 * Abort an in-progress wifi_connect() call from any task (e.g. a touch handler).
 * wifi_connect() will return ESP_FAIL and wifi_connect_was_aborted() will return true.
 */
void wifi_connect_abort(void);

/** Returns true if the last wifi_connect() was cut short by wifi_connect_abort(). */
bool wifi_connect_was_aborted(void);

/**
 * Initialise the TCP/IP stack, event loop, and WiFi driver (idempotent).
 * Must be called before esp_netif_create_default_wifi_ap/sta.
 */
void wifi_stack_init_public(void);
