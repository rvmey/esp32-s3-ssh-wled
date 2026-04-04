#pragma once

#include "esp_err.h"
#include <stdint.h>

/** Position / velocity / time data from a NAV-PVT UBX message. */
typedef struct {
    int32_t  lat;        /**< Latitude  in degrees × 1e-7 (WGS-84)   */
    int32_t  lon;        /**< Longitude in degrees × 1e-7 (WGS-84)   */
    int32_t  speed_mm_s; /**< Ground speed in mm/s                    */
    uint32_t unix_ts;    /**< UTC time as Unix timestamp (0 if unknown) */
    uint8_t  fix_type;   /**< 0=none 1=DR 2=2D 3=3D 4=GNSS+DR        */
    uint8_t  num_sv;     /**< Number of satellites used               */
} gps_pvt_t;

/**
 * Initialise the UART, optionally power the GPS module, configure the
 * u-blox receiver to output UBX NAV-PVT at 1 Hz.
 */
esp_err_t gps_ubx_init(void);

/**
 * Block until a NAV-PVT message with fixType >= 3 (3D fix) is received
 * or @p timeout_s seconds elapse.
 *
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no fix obtained.
 */
esp_err_t gps_ubx_wait_fix(int timeout_s);

/**
 * Fill @p pvt with the most recently received NAV-PVT data.
 * Returns ESP_ERR_NOT_FOUND if no valid fix has been received yet.
 */
esp_err_t gps_ubx_get_pvt(gps_pvt_t *pvt);

/**
 * Power down the GPS module (if a power GPIO is configured) and
 * uninstall the UART driver.
 */
void gps_ubx_deinit(void);
