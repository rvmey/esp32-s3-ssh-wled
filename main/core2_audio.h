#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t core2_audio_init(void);
void core2_audio_deinit(void);
/* Pause/resume — keep DMA descriptors alive; use around mic recording. */
void core2_audio_pause(void);
void core2_audio_resume(void);
/* Release/re-acquire only the I2S TX DMA channel, keeping the writer task and
 * ring alive (so no task is created/destroyed). Use around mic streaming during
 * a SIP call where mid-call task creation fails on fragmented internal SRAM. */
void core2_audio_release_dma(void);
esp_err_t core2_audio_acquire_dma(void);
/* Shrink the TX DMA footprint for SIP calls (true) or restore full size for
 * music (false). Re-creates the channel in place if one is open. */
void core2_audio_set_small_dma(bool small);
void core2_audio_set_sample_rate(uint32_t sample_rate_hz);
uint32_t core2_audio_get_sample_rate(void);
/* True while unplayed PCM is still queued in the ring buffer. Reports queued
 * data only — the last DMA chunk may still be draining after this goes false. */
bool core2_audio_is_busy(void);

/* sample_count is the total number of samples in input buffer, not frames. */
void core2_audio_write_pcm(const int16_t *samples,
                           size_t sample_count,
                           int channels,
                           int volume_percent);
