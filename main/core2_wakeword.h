#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * "Hi, ESP" wake-word detection (esp-sr WakeNet, wn9_hiesp model).
 *
 * A background task opportunistically owns the built-in PDM mic whenever the
 * speaker/mic hardware is otherwise idle (pf_wakeword_gate_open() in
 * picture_frame.c decides). GPIO 0 is shared between the PDM mic clock and
 * the speaker WS signal, so the speaker DMA is released while armed and
 * restored as soon as any audio activity starts. On detection the task
 * releases the mic and requests the normal voice-query flow — the same path
 * as a PWR-key single press or the "listen" command.
 *
 * The model blob lives in the "model" flash partition (srmodels.bin, packed
 * by esp-sr at build time from CONFIG_SR_WN_* selections).
 */

esp_err_t core2_wakeword_init(void);   /* load model + start the task */
void      core2_wakeword_set_enabled(bool enabled);
bool      core2_wakeword_get_enabled(void);

/* True while the wake-word task holds the mic (speaker DMA released). */
bool core2_wakeword_armed(void);

/* Synchronous barrier: forces the task to release the mic and restore the
 * speaker DMA before returning. Call at the start of any code path that will
 * touch the speaker or the mic, AFTER making that activity visible to
 * pf_wakeword_gate_open() (so the task doesn't immediately re-arm).
 * Returns instantly when not armed. */
void core2_wakeword_yield(void);
