# TRIGGERcmd CYD Display

This variant targets the ESP32-2432S028R ("Cheap Yellow Display") and reuses
the Core2 TRIGGERcmd display profile and Socket.IO workflow. It is a stripped
"display + system control" build: it registers only the commands that the
hardware can actually run (see [main/cyd_commands.json](main/cyd_commands.json)).

## Supported commands

| Command | What it does |
| --- | --- |
| `text` | Show text on the display |
| `color` | Set the background color |
| `textcolor` | Set the text color |
| `fontsize` | Set the font size (1–4) |
| `landscape` / `portrait` | Set screen orientation |
| `save` | Persist the current display settings to NVS |
| `folders` | List folders on the SD card |
| `files` | List files in an SD folder |
| `reboot` | Reboot the device |
| `sleeptimer` | Set inactivity minutes before deep sleep |
| `sleep` | Enter deep sleep now (wake by touch) |

## Why the CYD does less than the Core2

The single reason is **memory**. The CYD's ESP32 has **no PSRAM** and only
~40 KB of free internal SRAM at runtime (Wi-Fi, Bluetooth-classic, the display
driver, the SD stack, and the TLS stack all share it). The M5Stack Core2 has
**8 MB of PSRAM** plus extra peripherals, so it can do things the CYD simply
cannot fit:

- **One TLS connection at a time.** The persistent TRIGGERcmd Socket.IO
  connection is a TLS session, and a single TLS handshake already needs most of
  the largest contiguous free block on this board. There is no room for a
  *second* concurrent TLS context. Practical consequences:
  - Command **registration (sync) runs over the websocket** (Sails virtual
    POST), not over separate HTTPS calls, because a second HTTPS/TLS handshake
    while the websocket is open runs the heap out of memory and drops the
    command connection.
  - Anything that would open a second HTTPS connection (e.g. fetching a web
    image) is disabled — attempting it crashes the command link.

- **No image display.** Decoding even a 320×240 JPEG to RGB565 needs a ~150 KB
  output buffer, which cannot be allocated without PSRAM. Web image fetch is
  doubly impossible (it also needs the second TLS context above). So `jpeg`,
  `savepic`, and the per-folder picture commands are not available. *(The Core2
  decodes into PSRAM and fetches over HTTPS, so it supports all of these.)*

- **No audio / Bluetooth playback.** MP3 audio on the Core2 streams to a
  Bluetooth A2DP speaker through a 256 KB PCM ring buffer — again only possible
  with PSRAM. Bluetooth init is also skipped on the CYD for lack of heap
  headroom. So none of the playback/transport commands (`play`, `pause`,
  `next`, `previous`, `volumeup`/`down`/`level`, `mute`, `shuffle`, `repeat*`),
  the Bluetooth commands (`pair`, `btstatus`, `btdisconnect`, `btforget`), or
  the per-folder `music` commands are available.

- **No Core2-only peripherals.** The CYD lacks the hardware these commands
  drive, so they are omitted regardless of memory:
  - `visualizer`, `ledcolor` — the Core2's SK6812 side-LED bar
  - `battery` — the Core2's AXP192 power-management/battery gauge
  - `listen` — the Core2's microphone (voice capture)
  - `speak` — the Core2's speaker (text-to-speech); on the CYD it could only
    echo the text, so it is omitted as misleading.

In short: the CYD is a capable **text/Wi-Fi display and system-control**
endpoint, while the Core2 is a full **picture-frame + audio + voice** device.
If you need images, audio, or voice, use a Core2 (or another PSRAM board such as
the JC3248W535).

## Build

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant cyd
```

Output firmware path:

- `docs/firmware/esp32_cyd_picture_frame.bin`

## Installer manifest

- `docs/manifest-cyd.json`

## Notes

- CYD board-to-board wiring can vary by vendor. Use menuconfig under
  ESP32 SSH LED Configuration -> CYD Settings to adjust LCD SPI pins,
  optional LCD reset pin, and backlight GPIO polarity.
- Touch is optional and configurable in CYD Settings:
  - None
  - XPT2046 (SPI resistive)
  - CST816 (I2C capacitive)
