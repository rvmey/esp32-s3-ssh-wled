#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Core2 SPM1423 PDM microphone driver.
 *
 * GPIO 0  — PDM CLK  (shared with NS4168 speaker WS on I2S_NUM_1)
 * GPIO 34 — PDM DATA
 *
 * Call core2_audio_deinit() before core2_mic_init() and
 * core2_audio_init() after core2_mic_deinit() to avoid I2S GPIO conflicts.
 */

esp_err_t core2_mic_init(void);   /* returns ESP_ERR_NO_MEM if DMA alloc fails */
size_t    core2_mic_record(uint8_t **wav_out, uint32_t max_ms,
                          const volatile bool *stop_flag);
void      core2_mic_deinit(void);

/*
 * Streaming capture for live audio (e.g. SIP). core2_mic_init() must have been
 * called first. start enables the channel and flushes the DMA ring; read_frame
 * blocks for one chunk of 16 kHz mono PCM (HPF + gain applied, state persists
 * across frames within a stream); stop disables the channel (channel stays
 * installed — call core2_mic_deinit() to release GPIO 0 for the speaker).
 */
esp_err_t core2_mic_stream_start(void);
size_t    core2_mic_read_frame(int16_t *out, size_t max_samples);
void      core2_mic_stream_stop(void);
