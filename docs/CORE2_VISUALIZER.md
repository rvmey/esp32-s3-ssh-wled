# Core2 LED Audio Visualizer

The Core2 variant drives the 10 SK6812 LEDs on the sides of the M5Stack Core2
(GPIO 25) as a music visualizer while MP3 playback is active. Audio is
analyzed in real time and rendered to the LED strip in one of 20 styles.

---

## Physical layout

The 10-LED chain is split across the two side faces of the unit, 5 LEDs per
side:

- **LEDs 0–4** — left side face, ordered top → bottom (LED 0 is the
  topmost LED on the left side, LED 4 is the bottommost).
- **LEDs 5–9** — right side face, ordered bottom → top (LED 5 is the
  bottommost LED on the right side, LED 9 is the topmost).

```
LEFT SIDE (up)        RIGHT SIDE (down)

LED 0  ─ top-left       top-right ─ LED 9
LED 1                   LED 8
LED 2                   LED 7
LED 3                   LED 6
LED 4  ─ bottom-left    bottom-right ─ LED 5
```

So index 0 starts at the top-left corner and increases down the left side to
LED 4 at bottom-left, then LED 5 picks up at bottom-right and increases up to
LED 9 at top-right. This single logical chain
(`s_pixels[CORE2_LED_COUNT * 3]`) is what all five visualizer styles address.

> **Note:** this orientation was corrected based on hardware testing of
> [style 4](#style-4--mirrored-vu-meter) (v2.0.457). The earlier version of
> this diagram had top and bottom reversed. Style 1 uses the LED 0 → LED 4 /
> LED 9 → LED 5 fill order against this corrected orientation, which means it
> fills downward from the top — confirmed on hardware and kept intentionally
> (see [style 5](#style-5--vu-bars-bottom-up) for the bottom-up alternative).
> Style 2 was written against the old (incorrect) diagram and has **not** been
> re-verified against the corrected orientation; see the note in its section
> below.

---

## Enabling / controlling

| Command | Effect |
|---|---|
| `visualizer on` / `visualizer off` | Force the visualizer on or off |
| `visualizer` (no params) | Toggle on/off |
| `visualizer 1` | Select **style 1 — VU bars** (also turns the visualizer on) |
| `visualizer 2` | Select **style 2 — FFT spectrum** (also turns the visualizer on) |
| `visualizer 3` | Select **style 3 — chase** (also turns the visualizer on) |
| `visualizer 4` | Select **style 4 — mirrored VU meter** (also turns the visualizer on) |
| `visualizer 5` | Select **style 5 — VU bars (bottom-up)** (also turns the visualizer on) |
| `visualizer 6` | Select **style 6 — VU bars (mix)** (also turns the visualizer on) |
| `visualizer 7` | Select **style 7 — rainbow VU bars** (also turns the visualizer on) |
| `visualizer 8` | Select **style 8 — bass-pulse breathing** (also turns the visualizer on) |
| `visualizer 9` | Select **style 9 — dual mirrored mini-spectrum** (also turns the visualizer on) |
| `visualizer 10` | Select **style 10 — treble sparkle** (also turns the visualizer on) |
| `visualizer 11` | Select **style 11 — beat-flash strobe** (also turns the visualizer on) |
| `visualizer 12` | Select **style 12 — spectral-centroid comet** (also turns the visualizer on) |
| `visualizer 13` | Select **style 13 — center bloom** (also turns the visualizer on) |
| `visualizer 14` | Select **style 14 — color-temperature VU bars** (also turns the visualizer on) |
| `visualizer 15` | Select **style 15 — confetti** (also turns the visualizer on) |
| `visualizer 16` | Select **style 16 — rainbow wash** (also turns the visualizer on) |
| `visualizer 17` | Select **style 17 — peak-hold spectrum** (also turns the visualizer on) |
| `visualizer 18` | Select **style 18 — dominant-band spotlight** (also turns the visualizer on) |
| `visualizer 19` | Select **style 19 — pulsing VU bars** (also turns the visualizer on) |
| `visualizer 20` | Select **style 20 — mirrored chase** (also turns the visualizer on) |
| `visualizernext` | Switch to the next style, wrapping from 20 back to 1 (also turns the visualizer on) |
| `visualizerprevious` | Switch to the previous style, wrapping from 1 back to 20 (also turns the visualizer on) |

The on/off state and selected style are both persisted to NVS, so they
survive a reboot. Default style on a fresh install is **1**.

When the visualizer is off, `core2_leds_off()` blanks all 10 LEDs.

---

## Audio analysis (shared by both styles)

Regardless of style, the same analysis feeds both visualizers:

- While a track is playing, decoded PCM samples are fed into `viz_feed()`
  in 256-sample blocks (`VIZ_BLOCK`).
- Each block is run through 10 **Goertzel filters**, one per frequency band:

  ```
  60, 120, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000 Hz
  ```

- Each band tracks a decaying **peak level** (fast attack, ~0.88 decay per
  block) and the result is scaled/clamped to a `0.0–1.0` level per band.
- The resulting `levels[10]` array is handed to `core2_leds_set_bands()`
  (style 2) or reduced to two values and handed to `core2_leds_set_vu()`
  (style 1).

This all happens in `picture_frame.c` (`viz_init_for_rate`, `viz_run_block`,
`viz_feed`); the actual LED pixel output happens in `core2_leds.c`.

---

## Style 2 — FFT spectrum (the original visualizer)

Each of the 10 LEDs corresponds directly to one of the 10 frequency bands:

- LED 0 ↔ 60 Hz (bass) ... LED 9 ↔ 16 kHz (treble)
- Per the [physical layout](#physical-layout), this puts the bass end of the
  spectrum on the top-left LED of the left side and the treble end on the
  top-right LED of the right side — both ends of the spectrum sit at the
  top of the device.
- Each LED's color is a hue computed from its band index — red for bass,
  sweeping through the spectrum to violet for treble (`band_to_rgb()`).
- Each LED's **brightness** is driven by that band's current level — louder
  energy in that frequency range = brighter LED.

This gives a classic "spectrum analyzer" look: a rainbow strip where each
LED pulses independently with its own frequency band.

> **Unverified:** the "bass/treble at the top" placement above follows
> directly from the corrected [physical layout](#physical-layout), but the
> per-band mapping itself has not been checked against real hardware.

---

## Style 1 — VU-meter bars (new default)

Instead of mapping bands to individual LEDs, style 1 splits the strip into
two 5-LED zones and treats each as a **bar-graph level meter**:

- **Low zone (LEDs 0–4, the left side face):** driven by the loudest of the
  5 low-frequency bands (60 Hz – 1000 Hz). Fills starting from LED 0,
  extending toward LED 4.
- **High zone (LEDs 5–9, the right side face):** driven by the loudest of
  the 5 high-frequency bands (2000 Hz – 16000 Hz). Fills starting from
  LED 9, extending toward LED 5 (i.e. LED 9 lights first, then 8, 7...).

For each zone, the number of lit LEDs is proportional to that zone's level
(0–5 LEDs). Lit LEDs are colored with a VU-meter ramp based on their
position in the bar — green for the lower portion, yellow in the middle,
red at the top of the ramp — independent of which frequency drove them.

The effect: both zones behave like a classic bar-graph meter, growing
downward from the top of the device as the music gets louder — bass on the
left side, treble on the right. This fill direction is confirmed on real
hardware and is kept intentionally; see [style 5](#style-5--vu-bars-bottom-up)
for a bottom-up alternative using the same band split and color ramp.

Implemented as `core2_leds_set_vu(low_level, high_level)` in
`core2_leds.c`.

---

## Style 3 — Chase

A simple non-audio-reactive "marquee" animation: a single LED is lit at a
time, stepping from LED 0 through LED 9 and then wrapping back to LED 0. Each
time the chase completes a full lap of the strip, it switches to the next
color in a fixed palette (red, green, blue, yellow, cyan, magenta, repeating).

The step rate is paced by wall-clock time (one step every ~100 ms, i.e. one
lap per second), independent of the audio block rate.

Implemented as `core2_leds_set_chase(position, r, g, b)` in `core2_leds.c`,
driven from `viz_run_block()` in `picture_frame.c`.

---

## Style 4 — Mirrored VU meter

Like style 1, this splits the strip into two 5-LED zones, but instead of
the low/high frequency split, **both zones are driven by the same overall
loudness level** (the loudest of all 10 frequency bands) and mirror each
other:

- **Left zone (LEDs 0–4):** fills upward from LED 4 at the bottom of the
  left side toward LED 0 at the top.
- **Right zone (LEDs 5–9):** fills upward from LED 5 at the bottom of the
  right side toward LED 9 at the top (mirroring the left zone).

Note: on the physical hardware, LED 4 (left) / LED 5 (right) sit at the
bottom corners and LED 0 (left) / LED 9 (right) sit at the top corners —
the opposite of the orientation shown in the
[physical layout](#physical-layout) diagram above, which was found to be
reversed for styles 1/2 as well when this was tested with style 4.

The number of lit LEDs per zone ranges from 0 (silence) to 5 (loudest),
with mid-level loudness lighting roughly 2-3 LEDs on each side. The boundary
LED — the one currently transitioning between lit and unlit — is rendered at
partial **brightness** based on the fractional level, giving a smooth
gradient rather than a hard step. All lit LEDs are red.

Implemented as `core2_leds_set_vu_mirror(level)` in `core2_leds.c`.

---

## Style 5 — VU bars (bottom-up)

This style reuses style 1's low/high band split and color ramp exactly, but
fills the strip in the opposite direction — bottom-up instead of top-down:

- **Low zone (LEDs 0–4, the left side face):** driven by the loudest of the
  5 low-frequency bands (60 Hz – 1000 Hz). Fills starting from LED 4 at the
  bottom-left corner, extending toward LED 0 at the top-left corner.
- **High zone (LEDs 5–9, the right side face):** driven by the loudest of
  the 5 high-frequency bands (2000 Hz – 16000 Hz). Fills starting from
  LED 5 at the bottom-right corner, extending toward LED 9 at the top-right
  corner.

As with style 1, the number of lit LEDs per zone is proportional to that
zone's level (0–5 LEDs), and lit LEDs use the same green/yellow/red VU-meter
ramp based on their position in the bar.

The result: both zones grow upward from the bottom of the device as the
music gets louder — the inverse of style 1's top-down fill — bass on the
left side, treble on the right.

Implemented as `core2_leds_set_vu_bottomup(low_level, high_level)` in
`core2_leds.c`.

---

## Style 6 — VU bars (mix)

This style reuses the same low/high band split as styles 1 and 5, but
combines a top-down fill and a bottom-up fill on **each** side face at the
same time, in different colors:

- **Highs (blue) fill downward from the top:** LED 0 → LED 4 on the left
  side, LED 9 → LED 5 on the right side, lit count proportional to the
  high-frequency level (2000 Hz – 16000 Hz).
- **Lows (red) fill upward from the bottom:** LED 4 → LED 0 on the left
  side, LED 5 → LED 9 on the right side, lit count proportional to the
  low-frequency level (60 Hz – 1000 Hz).

Both fills are applied to both side faces (mirrored), so as the music gets
louder the blue fill grows down from the top corners and the red fill grows
up from the bottom corners. Where the two fills overlap in the middle of a
side, the LED shows both colors mixed together (magenta).

Implemented as `core2_leds_set_vu_mix(low_level, high_level)` in
`core2_leds.c`.

---

## Style 7 — Rainbow VU bars

Same fill positions as style 1 (low band fills LEDs 0–4 from LED 0, high band
fills LEDs 5–9 from LED 9), but instead of the green/yellow/red ramp, each lit
LED's color comes from a rainbow gradient that slowly rotates over time
(60°/sec). The result looks like style 1's bars, but the colors continuously
cycle through the spectrum rather than encoding loudness.

Implemented in `viz_run_block()` (`picture_frame.c`) using
`core2_leds_hsv_to_rgb()` + `core2_leds_set_pixels_rgb()`.

---

## Style 8 — Bass-pulse breathing

The simplest style: all 10 LEDs show the same color at all times. Brightness
tracks the bass level (the loudest of the 60/120/250 Hz bands), so the whole
strip "breathes" with the beat. The hue slowly rotates over time (20°/sec)
independent of the audio, so the breathing color cycles through the rainbow
over the course of a song.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` +
`core2_leds_set_pixels_rgb()`.

---

## Style 9 — Dual mirrored mini-spectrum

Each side face acts as its own 5-band spectrum analyzer:

- **Left zone (LEDs 0–4):** bands 0–4 (60 Hz – 1000 Hz), LED 0 = band 0
  (bass) … LED 4 = band 4.
- **Right zone (LEDs 5–9):** bands 9–5 (16000 Hz down to 1000 Hz), LED 5 =
  band 9 (treble) … LED 9 = band 5.

Each LED's hue comes from its band's position in the global 10-band spectrum
(red = bass … violet = treble, same hue mapping as style 2) and its
brightness comes from that band's level. The two zones effectively show
mirrored halves of the spectrum.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` +
`core2_leds_set_pixels_rgb()`.

---

## Style 10 — Treble sparkle

All 10 LEDs show a dim blue base whose brightness tracks the bass level
(60 Hz – 1000 Hz). On top of that, a number of randomly chosen LEDs flash
bright white each block — the number of sparkles is proportional to the
treble level (8000/12000/16000 Hz bands), so busy high-frequency content
("ts ts ts" hi-hats, cymbals) produces a flurry of white sparkles over a dim
blue glow.

Implemented in `viz_run_block()` using a small xorshift PRNG
(`viz_rand()`), `core2_leds_hsv_to_rgb()`, and `core2_leds_set_pixels_rgb()`.

---

## Style 11 — Beat-flash strobe

Tracks a slow moving average of the bass level (60/120/250 Hz). When the
instantaneous bass level spikes well above that average — a "kick" — all 10
LEDs flash white at full brightness, then decay back over roughly 200 ms.
Between kicks, the strip shows a dim white glow proportional to the overall
level. This gives a strobe-like effect synced to bass drum hits rather than
to any fixed frequency band's continuous level.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` (saturation
0 = white) + `core2_leds_set_pixels_rgb()`.

---

## Style 12 — Spectral-centroid comet

Computes the **spectral centroid** each block — the level-weighted average of
the 10 band indices, which shifts toward 0 (bass end) for bass-heavy audio and
toward 9 (treble end) for treble-heavy audio. A bright "comet" is drawn at
that position (split across the two nearest LEDs by the fractional part),
leaving a fading trail behind it (each LED's trail brightness decays by 25%
per block). Each LED position has a fixed hue along the same red→violet ramp
used by style 2, and the comet's brightness scales with the overall level — so
quiet passages produce a faint trail and loud, bass-or-treble-heavy passages
produce a bright comet that visibly slides toward that end of the strip.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` +
`core2_leds_set_pixels_rgb()`.

---

## Style 13 — Center bloom

Each 5-LED zone blooms outward from its center LED (LED 2 on the left, LED 7
on the right) toward both ends as the overall level rises — 0 LEDs lit at
silence, growing through the center LED, then ±1, then ±2 (all 5) at full
level. Both zones bloom in the same color, which is the hue of whichever
frequency band is currently loudest (the "dominant band"), so the bloom's
color shifts with the dominant frequency while its size tracks overall
loudness.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` +
`core2_leds_set_pixels_rgb()`.

---

## Style 14 — Color-temperature VU bars

Same corner-fill positions as style 1 (low band fills LEDs 0–4 from LED 0,
high band fills LEDs 5–9 from LED 9), but instead of a fixed green/yellow/red
ramp, each zone's lit LEDs share a single color whose **hue** is derived from
that zone's level: blue (240°) at low levels sweeping to red (0°) at high
levels. The two zones are colored independently, so e.g. a bass-heavy moment
can show a "hot" red low zone next to a "cool" blue high zone.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` +
`core2_leds_set_pixels_rgb()`.

---

## Style 15 — Confetti

A non-deterministic sparkle effect: every ~30 ms, all LEDs' brightness decays
by 15%, and then a number of new sparkles are spawned proportional to the
overall level (0–2 per tick). Each spawned sparkle picks a random LED and a
random hue and sets it to full brightness, which then fades out over the
following ticks. The result is a field of randomly-colored sparkles whose
spawn rate rises and falls with the music.

Implemented in `viz_run_block()` using `viz_rand()`, `core2_leds_hsv_to_rgb()`,
and `core2_leds_set_pixels_rgb()`.

---

## Style 16 — Rainbow wash

A non-audio-reactive rainbow gradient is spread evenly across all 10 LEDs
(36° of hue between adjacent LEDs, one full rainbow around the strip) and
continuously rotates over time (90°/sec). The only audio-reactive element is
overall brightness, which breathes between 15% and 100% with the overall
level — so the rainbow pattern itself is constant, but it dims during quiet
passages and brightens during loud ones.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` +
`core2_leds_set_pixels_rgb()`.

---

## Style 17 — Peak-hold spectrum

Like style 2, LED *i* corresponds directly to band *i* (red = bass … violet =
treble). In addition, each band tracks a slowly-decaying **peak** value
(`peak *= 0.93` per block, raised to the current level whenever it's
exceeded). Each LED's brightness is the greater of the current level and 60%
of that band's peak, so a band that was recently loud keeps a dim trailing
glow for a moment after it quiets down — the classic "peak hold" behavior of
hardware equalizer displays.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` +
`core2_leds_set_pixels_rgb()`.

---

## Style 18 — Dominant-band spotlight

Each block, the band with the highest level (the "dominant band") is found.
The single LED corresponding to that band (LED *i* ↔ band *i*, as in style 2)
lights up brightly with that band's hue; every other LED stays at a very dim
white glow. As the dominant frequency shifts during a track, the bright
"spotlight" jumps to a different position along the strip.

Implemented in `viz_run_block()` using `core2_leds_hsv_to_rgb()` +
`core2_leds_set_pixels_rgb()`.

---

## Style 19 — Pulsing VU bars

Same low/high zone fill and green/yellow/red ramp as style 1, but with an
extra global brightness multiplier (40%–100%) driven by the overall level.
Quiet passages dim the entire bar display down to 40% brightness; loud
passages bring it back to full — layering a "master volume pulse" on top of
style 1's per-zone bar logic.

Implemented in `viz_run_block()` using `core2_leds_set_pixels_rgb()`.

---

## Style 20 — Mirrored chase

Like style 3's chase, but two LEDs move together: LED *i* and its mirror
LED `9-i` step in lockstep around the strip (covering all 10 LEDs over 5
steps), changing to the next color in the same 6-color palette as style 3
each time they complete a lap. The step interval is driven by the overall
level — from 180 ms at silence down to 60 ms at full loudness — so the chase
speeds up with the music.

Implemented in `viz_run_block()` using `core2_leds_set_pixels_rgb()`.

---

## Implementation notes

- `CORE2_LED_COUNT` = 10, `CORE2_LED_GPIO` = 25 (`core2_leds.h`).
- LED output uses a direct RMT driver with SK6812/WS2812-compatible timing
  (`core2_leds.c`); no `led_strip` component dependency.
- `s_pixels[CORE2_LED_COUNT * 3]` is a static GRB pixel buffer reused by
  both styles — `core2_leds_set_bands()`, `core2_leds_set_vu()`, and
  `core2_leds_set_solid()` (used by `ledcolor`) all write into it and flush
  via the same `led_flush()`.
- Styles 7–20 are implemented directly in `viz_run_block()`
  (`picture_frame.c`) rather than as dedicated `core2_leds_set_*()`
  functions. They compute a flat R,G,B buffer and push it with the generic
  `core2_leds_set_pixels_rgb()`, using `core2_leds_hsv_to_rgb()` for color
  and a small xorshift PRNG (`viz_rand()`) for the sparkle/confetti styles.
- Several shared per-block aggregates (`v_low`, `v_high`, `v_overall`,
  `v_dom` — the dominant band index, and `v_centroid` — the level-weighted
  average band index) are computed once at the top of `viz_run_block()` and
  reused across styles 7–20.
