#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * atom_mic — PDM microphone capture for the M5Stack ATOM Echo (SPM1423).
 *
 * The SPM1423 is driven via the ESP32 I²S peripheral in PDM-RX mode on
 * I2S_NUM_0. GPIO33 is shared with the speaker (NS4168) LRCK, but once
 * the mic is initialized, GPIO33 routes to I2S0 PDM clock and stays there.
 *
 * PDM pins: CLK=GPIO33 (shared with speaker), DATA=GPIO23
 * Sample rate: 16 kHz, 16-bit, mono.
 *
 * Memory note: the classic ESP32 has ~520 KB SRAM shared with WiFi buffers,
 * TLS stack, etc. Recording is capped at 4 seconds (128 KB PCM + 44-byte
 * WAV header) to stay within practical heap limits.
 */

/** Install the I²S PDM-RX driver. */
void atom_mic_init(void);

/**
 * Record audio until the button GPIO goes HIGH (released) or max_ms elapses,
 * whichever comes first.  Assembles a standard 44-byte WAV header and returns
 * a heap-allocated buffer containing the complete WAV file.
 *
 * @param wav_out   Output pointer; set to the allocated WAV buffer on success.
 *                  Caller must free() this buffer.
 * @param button_gpio  GPIO number of the button (active-low); recording stops
 *                     when this pin reads HIGH.
 * @param max_ms    Maximum recording duration in milliseconds (capped at 4000).
 * @return          Number of bytes in *wav_out, or 0 on failure.
 */
size_t atom_mic_record(uint8_t **wav_out, int button_gpio, uint32_t max_ms);

/** Uninstall the I²S PDM-RX driver to free memory when not recording. */
void atom_mic_deinit(void);
