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
size_t    core2_mic_record(uint8_t **wav_out, uint32_t max_ms);
void      core2_mic_deinit(void);
