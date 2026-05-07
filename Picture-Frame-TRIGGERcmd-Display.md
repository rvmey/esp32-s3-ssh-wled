# Picture Frame - TRIGGERcmd Display

This variant targets the **Guition JC3248W535** (ESP32-S3 + 320x480 AXS15231
screen) and works as a cloud-connected **TRIGGERcmd display endpoint**.

It does not use the SSH command shell. Instead, it connects to TRIGGERcmd,
receives commands over Socket.IO, and updates the display in real time.

---

## What it does

1. Initializes the display and connects to Wi-Fi.
2. If needed, starts BLE Improv Wi-Fi provisioning.
3. Pairs to TRIGGERcmd with a pair code flow (`/pair` + `/pair/lookup`).
4. Creates/registers a computer identity (`TCMDSCREEN-<MAC>`).
5. Connects to Socket.IO and subscribes to its TRIGGERcmd room.
6. Executes incoming display commands and reports completion (`/api/run/save`).

---

## Supported TRIGGERcmd commands

| Command | Parameters | Effect |
|---------|------------|--------|
| `text` | message text | Draw wrapped text on screen |
| `color` | named color or hex (`#RRGGBB`) | Set background color |
| `textcolor` | named color or hex (`#RRGGBB`) | Set text color |
| `fontsize` | integer | Set font scale (clamped to 1-4) |
| `landscape` | none | Rotate to 480x320 |
| `portrait` | none | Rotate to 320x480 |
| `jpeg` | image URL | Download/decode/display JPEG |
| `save` | none | Persist current display state to NVS |

### Notes

- JPEG downloads are capped at **512 KB** and decoded to RGB565.
- Last JPEG is cached so orientation changes can redraw without re-downloading.
- The `save` command stores display state (colors, font, orientation, text, JPEG URL)
  in NVS and restores it on reboot.

## Example: sending a kitty photo via ChatGPT

Ask ChatGPT to look up a public cat image and send it to the display using the TRIGGERcmd app:

![ChatGPT sending a kitty JPEG URL via the TRIGGERcmd app](chatgpt_triggercmd_kitty.png)

The Picture Frame receives the `jpeg` command over Socket.IO and immediately downloads and renders the image:

![Picture frame displaying the kitty photo](picture_frame_kitty.jpg)

---

## Display states during boot/connect

| State | Screen behavior |
|-------|------------------|
| Waiting for Wi-Fi / provisioning | Status text shown; blue background for BLE provisioning |
| Wi-Fi connected | Brief green status then display off |
| Pairing | Shows host and pair code instructions |
| Connecting to TRIGGERcmd | "Connecting to server..." |
| Ready | "Connected! Waiting for commands..." |

---

## Hardware notes

| Item | Detail |
|------|--------|
| Board | Guition JC3248W535 |
| Display | AXS15231 QSPI TFT, 320x480 |
| Console | USB Serial/JTAG |
| Memory | OPI PSRAM enabled (needed for JPEG buffers) |

---

## Building this variant

In `menuconfig`, under **ESP32 SSH LED Configuration**, select:

> **Hardware variant -> TriggerCMD Picture Frame (JC3248W535 + Socket.IO commands)**

Build with the project script:

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant picture_frame
```

Output firmware path:

- `docs/firmware/esp32_picture_frame.bin`

---

## Related files

- `main/picture_frame.c` - main firmware flow for this variant
- `sdkconfig.picture_frame` - variant configuration
- `docs/manifest-picture_frame.json` - browser installer manifest
