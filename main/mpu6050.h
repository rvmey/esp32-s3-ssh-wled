#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialise the I2C master bus and the MPU6050.
 * Switches the device to full-power mode with gyroscope disabled.
 * Must be called after every deep-sleep wakeup (peripherals are reset).
 */
esp_err_t mpu6050_init(void);

/**
 * Put the MPU6050 into low-power cycle mode (5 Hz sampling) with the
 * motion-detect interrupt armed. Call this just before deep sleep.
 */
esp_err_t mpu6050_configure_wakeup(void);

/**
 * Read INT_STATUS to clear the latched motion interrupt. Must be called
 * after wakeup and again before entering deep sleep to avoid an
 * immediate re-wakeup.
 */
void mpu6050_clear_interrupt(void);

/**
 * Return true if a motion interrupt has fired since the last call to
 * mpu6050_clear_interrupt(). Reading INT_STATUS clears the latch.
 * Used during TRACKING to detect inactivity.
 */
bool mpu6050_is_active(void);
