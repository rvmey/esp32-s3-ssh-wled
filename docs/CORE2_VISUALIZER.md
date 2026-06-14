# Core2 LED Audio Visualizer

The Core2 variant drives the 10 SK6812 LEDs on the sides of the M5Stack Core2
(GPIO 25) as a music visualizer while MP3 playback is active. Audio is
analyzed in real time and rendered to the LED strip in one of four styles.

---

## Physical layout

The 10-LED chain is split across the two side faces of the unit, 5 LEDs per
side:

- **LEDs 0–4** — left side face, ordered bottom → top (LED 0 is the
  bottommost LED on the left side, LED 4 is the topmost).
- **LEDs 5–9** — right side face, ordered top → bottom (LED 5 is the
  topmost LED on the right side, LED 9 is the bottommost).

```
LEFT SIDE (up)        RIGHT SIDE (down)

LED 4  ─ top-left       top-right ─ LED 5
LED 3                   LED 6
LED 2                   LED 7
LED 1                   LED 8
LED 0  ─ bottom-left    bottom-right ─ LED 9
```

So index 0 starts at the bottom-left corner and increases up the left side to
LED 4 at top-left, then LED 5 picks up at top-right and decreases down to
LED 9 at bottom-right. This single logical chain
(`s_pixels[CORE2_LED_COUNT * 3]`) is what both visualizer styles address.

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
  spectrum on the bottom-left LED of the left side and the treble end on the
  bottom-right LED of the right side — both ends of the spectrum sit at the
  bottom of the device.
- Each LED's color is a hue computed from its band index — red for bass,
  sweeping through the spectrum to violet for treble (`band_to_rgb()`).
- Each LED's **brightness** is driven by that band's current level — louder
  energy in that frequency range = brighter LED.

This gives a classic "spectrum analyzer" look: a rainbow strip where each
LED pulses independently with its own frequency band.

---

## Style 1 — VU-meter bars (new default)

Instead of mapping bands to individual LEDs, style 1 splits the strip into
two 5-LED zones and treats each as a **bar-graph level meter**:

- **Low zone (LEDs 0–4, the left side face):** driven by the loudest of the
  5 low-frequency bands (60 Hz – 1000 Hz). Fills upward starting from LED 0
  at the bottom of the left side, extending toward the top.
- **High zone (LEDs 5–9, the right side face):** driven by the loudest of
  the 5 high-frequency bands (2000 Hz – 16000 Hz). Fills upward starting
  from LED 9 at the bottom of the right side, extending toward the top (i.e.
  LED 9 lights first, then 8, 7...).

For each zone, the number of lit LEDs is proportional to that zone's level
(0–5 LEDs). Lit LEDs are colored with a VU-meter ramp based on their
position in the bar — green for the lower portion, yellow in the middle,
red at the top — independent of which frequency drove them.

The effect: both zones behave like a classic bar-graph meter, growing
upward from the bottom of the device as the music gets louder — bass on the
left side, treble on the right.

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

## Implementation notes

- `CORE2_LED_COUNT` = 10, `CORE2_LED_GPIO` = 25 (`core2_leds.h`).
- LED output uses a direct RMT driver with SK6812/WS2812-compatible timing
  (`core2_leds.c`); no `led_strip` component dependency.
- `s_pixels[CORE2_LED_COUNT * 3]` is a static GRB pixel buffer reused by
  both styles — `core2_leds_set_bands()`, `core2_leds_set_vu()`, and
  `core2_leds_set_solid()` (used by `ledcolor`) all write into it and flush
  via the same `led_flush()`.
