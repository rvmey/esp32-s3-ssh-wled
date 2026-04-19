#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * atom_audio — I²S speaker driver for the M5Stack ATOM Echo (NS4168 amp).
 *
 * I²S pins:  BCK=GPIO19, LRCK=GPIO33, DOUT=GPIO22
 * Sample rate: 16 kHz, 16-bit, mono (left channel only).
 *
 * GPIO33 is shared with the microphone PDM clock. The app deinitializes
 * speaker I2S before recording and reinitializes it afterward.
 *
 * PCM clips recorded at 8 kHz are upsampled 2× by duplicating each sample
 * before being written to I²S.
 */

/** Install the I²S driver and configure the NS4168 output. */
void atom_audio_init(void);

/** Uninstall the I²S driver to release GPIO33 for microphone PDM clock. */
void atom_audio_deinit(void);

/**
 * Play a PCM clip.
 * @param samples      Signed 16-bit PCM samples (mono).
 * @param num_samples  Number of samples.
 * @param sample_rate  Original sample rate of the clip (e.g. 8000 or 16000).
 *                     If sample_rate < 16000, each sample is duplicated to
 *                     upsample to 16 kHz.
 */
void atom_audio_play_clip(const int16_t *samples, size_t num_samples,
                          uint32_t sample_rate);

/** Play an ascending two-tone beep (success / health-check OK). */
void atom_audio_beep_ok(void);

/** Play a descending two-tone beep (failure). */
void atom_audio_beep_fail(void);
