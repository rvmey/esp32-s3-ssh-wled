#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * A single GPS track-point stored in NVS.
 * Lat/lon are degrees × 1e-7 (WGS-84).
 * speed_kmh is km/h × 10 (e.g. 245 = 24.5 km/h).
 */
typedef struct {
    int32_t  lat;
    int32_t  lon;
    int16_t  speed_kmh;
    uint16_t _pad;          /* reserved, must be zero */
} track_point_t;            /* 12 bytes               */

/**
 * Initialise the bike_nvs NVS partition.
 * Must be called once before any other ride_log_* function.
 */
esp_err_t ride_log_init(void);

/**
 * Begin a new ride at Unix timestamp @p start_ts.
 * Allocates the RAM accumulation buffer.
 */
esp_err_t ride_log_start(uint32_t start_ts);

/**
 * Append one track-point to the current ride.
 * Automatically flushes to NVS when the RAM buffer is full.
 */
esp_err_t ride_log_append(const track_point_t *pt);

/**
 * Flush remaining buffered points to NVS and close the current ride.
 * Returns the zero-based index of the completed ride.
 */
uint32_t ride_log_finish(void);

/** Return the total number of rides ever started (monotonic counter). */
esp_err_t ride_log_count(uint32_t *count);

/**
 * Read all track-points for ride @p idx.
 * On success, *pts is heap-allocated — caller must free(*pts).
 */
esp_err_t ride_log_read(uint32_t idx, track_point_t **pts,
                        size_t *n_pts, uint32_t *start_ts);

/** Erase all NVS keys belonging to ride @p idx. */
esp_err_t ride_log_delete(uint32_t idx);
