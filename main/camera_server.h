#pragma once

/**
 * Run the ESP32-S3 camera variant (Seeed XIAO ESP32-S3 Sense, OV2640).
 *
 * Provisions / connects WiFi, initialises the OV2640, and starts an HTTP
 * server that returns the latest JPEG frame at GET /cam.jpg so a Core2 (or any
 * client) on the same LAN can fetch and display the live view.
 *
 * This function blocks forever and never returns.
 */
void camera_server_run(void);
