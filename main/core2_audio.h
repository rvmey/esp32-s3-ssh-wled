#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t core2_audio_init(void);
void core2_audio_deinit(void);
void core2_audio_set_sample_rate(uint32_t sample_rate_hz);

/* sample_count is the total number of samples in input buffer, not frames. */
void core2_audio_write_pcm(const int16_t *samples,
                           size_t sample_count,
                           int channels,
                           int volume_percent);
