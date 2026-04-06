#pragma once

#include "esp_err.h"

/**
 * @brief Start the TRIGGERcmd pairing-info HTTP server (port 80).
 *
 * Serves a read-only page at GET / displaying @p pair_code with step-by-step
 * instructions for the user to visit www.triggercmd.com and enter the code.
 * If the server is already running, only the displayed code is updated.
 *
 * A POST /reprovision endpoint erases "computer_id" from NVS and reboots,
 * allowing the device to be reassigned to a different account.
 *
 * @param pair_code  5-character uppercase pair code, e.g. "AB3X7"
 */
esp_err_t http_pf_config_start(const char *pair_code);

/**
 * @brief Stop the TRIGGERcmd pairing-info HTTP server.
 *
 * Safe to call even if the server is not currently running.
 */
void http_pf_config_stop(void);
