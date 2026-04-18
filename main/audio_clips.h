#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * audio_clips — Embedded 8 kHz 16-bit mono PCM clips, one per alphanumeric
 * character (digits 0–9, letters A–Z).
 *
 * Stub arrays are empty (0 samples) until the developer runs the generation
 * script (see docs/TCMD_ATOM_ECHO.md — Phase 5) to produce and embed the
 * real clips.  The firmware will silently skip playback of any stub clip.
 *
 * Generation workflow per character:
 *   espeak "<char>" --stdout \
 *     | ffmpeg -i pipe:0 -ar 8000 -ac 1 -f s16le clip_<char>.raw
 *   xxd -i clip_<char>.raw  >>  audio_clips.c   (then adjust array name)
 */

#define AUDIO_CLIP_SAMPLE_RATE  8000u

typedef struct {
    const int16_t *samples;
    size_t         num_samples;
} audio_clip_t;

/*
 * Return the clip for ASCII character c.
 * Accepts '0'–'9', 'A'–'Z', 'a'–'z' (lower-case mapped to upper).
 * Returns {NULL, 0} for any other character.
 */
audio_clip_t clip_for_char(char c);
