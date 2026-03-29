#pragma once

#include "esp_err.h"

/**
 * Start the HTTP configuration server.
 * Serves a setup page at http://<device-ip>/ that lets the user change the
 * SSH password.  The new password is persisted to NVS and takes effect for
 * the next SSH login.
 *
 * Call this after WiFi is connected.
 */
esp_err_t http_config_start(void);
