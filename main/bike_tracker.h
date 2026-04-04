#pragma once

/**
 * Run the bike tracker firmware.  This function never returns — it ends
 * by calling esp_deep_sleep_start().
 *
 * Expected call site (after NVS init):
 *   #if CONFIG_HARDWARE_BIKE_TRACKER
 *       bike_tracker_run();
 *       return;
 *   #endif
 */
void bike_tracker_run(void);
