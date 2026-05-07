# ESP32-S3-DevKitC-1 — Onboard RGB LED via SSH

This variant targets the **ESP32-S3-DevKitC-1** development board and controls
the onboard **WS2812 RGB LED on GPIO 48** over an SSH connection powered by
[wolfSSH](https://www.wolfssl.com/wolfssh/).

---

## Shell commands

```
> help

Commands:
  color <name>      Named colour: red green blue white yellow
                    cyan magenta purple orange pink
  color R G B       RGB triplet, each value 0-255
  color #RRGGBB     Hex colour (e.g. #FF8800)
  off               Turn the LED off
  status            Show current LED colour
  help              Show this help text
  exit | quit       Close the connection
```

### Examples

```
> color red
LED set to R:255 G:0   B:0    (#FF0000)

> color 0 128 255
LED set to R:0   G:128 B:255  (#0080FF)

> color #FF8800
LED set to R:255 G:136 B:0    (#FF8800)

> off
LED off.

> status
LED is off.
```

---

## LED boot indicators

| Colour | Meaning |
|--------|---------|
| Brief blue | Wi-Fi provisioning or connecting |
| Brief green | Wi-Fi connected, IP obtained |
| Solid red | Error (Wi-Fi failed or SSH init failed) — check serial log |

---

## Hardware notes

| Signal | GPIO |
|--------|------|
| WS2812 data | 48 |

The WS2812 is driven by the ESP32-S3 RMT peripheral via the
`espressif/led_strip` component.

---

## Building this variant

In `menuconfig`, under **"ESP32 SSH LED Configuration"**, select:

> **Hardware variant → ESP32-S3-DevKitC-1 (onboard WS2812 RGB LED on GPIO 48)**

Then build normally:

```bash
idf.py build
idf.py -p COM<N> flash
```

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| Wrong LED color / no output | Wrong pixel format — change `LED_PIXEL_FORMAT_GRB` ↔ `LED_PIXEL_FORMAT_RGB` in `led_control.c` |
| LED flickers | Marginal 5 V supply; try a different USB cable or hub |
