#pragma once

#include "esp_err.h"

/**
 * Initialise the MPU6886 over the shared I2C bus (I2C_NUM_0, already
 * installed by screen_control_core2.c).  Performs a device reset, verifies
 * WHO_AM_I (0x19), and puts the chip in normal power mode with gyro standby.
 * Must be called after every deep-sleep wakeup.
 */
esp_err_t mpu6886_init(void);

/**
 * Arm the Wake-on-Motion interrupt and enter low-power cycle mode (5 Hz).
 * Call this immediately before esp_deep_sleep_start().
 *
 * @param threshold  Motion detection threshold in WOM LSBs (1 LSB ≈ 4 mg).
 *                   0x20 (128 mg) is a good starting value for pick-up detection.
 */
esp_err_t mpu6886_configure_wom(uint8_t threshold);

/**
 * Read INT_STATUS to deassert the latched INT output.  Must be called after
 * waking from deep sleep so GPIO 35 goes low before being released as EXT1.
 */
void mpu6886_clear_interrupt(void);
