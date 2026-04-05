#pragma once

/**
 * @brief Run the picture-frame firmware loop.
 *
 * Initialises the display, waits for WiFi, provisions with TriggerCMD,
 * then connects a Socket.IO session and forwards incoming "display" events
 * to the screen.  Never returns.
 */
void picture_frame_run(void);
