#pragma once

#include "esp_err.h"

/**
 * @brief Start the TRIGGERcmd hardware-token setup HTTP server (port 80).
 *
 * Serves a setup form at http://<device-ip>/ where the user enters their
 * TRIGGERcmd hardware JWT.  On POST /provision the token (and optional
 * computer ID) is stored to NVS namespace "pf_cfg" and the device reboots.
 *
 * A POST /reprovision endpoint erases only the "computer_id" key and reboots,
 * forcing the firmware to fetch a fresh computer ID from the provision API.
 *
 * Call this after WiFi is connected and only when hw_token is absent from NVS.
 */
esp_err_t http_pf_config_start(void);
