#pragma once

#include "esp_err.h"

/**
 * Upload all rides stored in NVS to CONFIG_TRACKER_UPLOAD_URL via HTTP POST.
 *
 * Each ride is serialised as:
 *   {"start_ts":<unix>,"points":[{"lat":<v>,"lon":<v>,"speed_kmh":<v>},...]}
 *
 * A ride is deleted from NVS only after a 200 or 201 response is received.
 * If the URL is empty, or WiFi is unavailable, the function returns ESP_OK
 * without doing anything (rides are preserved for the next opportunity).
 */
esp_err_t ride_upload_all(void);
