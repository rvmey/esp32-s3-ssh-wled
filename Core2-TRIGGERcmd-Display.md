# M5Stack Core2 for AWS — TRIGGERcmd Display

This variant runs the TRIGGERcmd picture-frame firmware on the **M5Stack
Core2 for AWS** (classic ESP32, ILI9342C 320×240 SPI display, AXP192 power
management, 8 MB PSRAM, 10x SK6812 side LEDs). It shares its Socket.IO
pairing/registration flow and core display commands with the
[Picture Frame](Picture-Frame-TRIGGERcmd-Display.md) (JC3248W535) variant,
but the Core2's extra hardware — PSRAM, battery gauge, Bluetooth, microphone,
speaker, and side LED bar — unlocks a much larger command set. It's the most
fully-featured device in this repo.

It does not use the SSH command shell. It connects to TRIGGERcmd over
Socket.IO, receives commands, and updates the display, audio, and LEDs in
real time.

---

## Major features

| Feature | Highlights |
|---|---|
| **Picture frame** | Show JPEGs from a URL (`jpeg`), save to SD (`savepic`), browse/display per-folder pictures, persist display state across reboots (`save`) |
| **Bluetooth MP3 player** | Stream MP3s from the SD card to a paired Bluetooth speaker/headset — play/pause/skip/seek/volume/shuffle/repeat, per-folder playlists, live now-playing screen |
| **LED music visualizer** | 100 audio-reactive styles on the 10-LED SK6812 side bar, synced to MP3 playback |
| **AI voice assistant** | Push-to-talk voice queries (Whisper STT), spoken AI replies (TTS), and "ask about this picture" vision Q&A |
| **Battery & power** | Battery level/voltage/charging status, configurable auto-sleep on battery, wake by touch |
| **Bluetooth pairing** | Pair/forget/status for the audio output device |

---

## Display & pictures

| Command | Effect |
|---|---|
| `text` | Draw wrapped text on screen |
| `color` / `textcolor` | Set background / text color (named or `#RRGGBB`) |
| `fontsize` | Set font scale (1-4) |
| `landscape` / `portrait` | Rotate the display (defaults to landscape — the Core2's panel is mounted landscape) |
| `jpeg` | Download and display a JPEG from a URL |
| `savepic` | Save the currently displayed JPEG to `/saved-jpegs` on the SD card |
| `folders` / `files` | List SD card folders / files in a folder |
| Per-folder picture commands | Auto-generated for each folder of JPEGs on the SD card — display the Nth (or first/random) image in that folder |
| Per-folder music commands | Auto-generated for each folder of MP3s on the SD card — play the Nth (or first/random, if shuffle is on) track in that folder (see [Bluetooth MP3 playback](#bluetooth-mp3-playback)) |
| `save` | Persist display state (colors, font, orientation, text, JPEG URL) to NVS and, if an SD card is present, to `/sdcard/core2_settings.cfg`; restored on reboot (SD takes priority over NVS) |
| `askpic` | Ask an AI vision question about the currently displayed picture |

---

## Bluetooth MP3 playback

The Core2 streams MP3 files from the SD card to a paired Bluetooth A2DP
speaker/headset through a 256 KB PCM ring buffer (PSRAM-backed).

| Command | Effect |
|---|---|
| `play` / `pause` / `stop` | Resume / toggle / pause playback |
| `next` / `previous` | Skip to the next/previous track in the folder |
| `forward` / `reverse` | Seek ±10 seconds |
| `volumeup` / `volumedown` / `volumelevel` | Adjust volume, or set an exact 0-100% level |
| `mute` | Toggle mute (not persisted across reboots) |
| `shuffle`, `repeattrack`, `repeatplaylist` | Toggle playback modes |
| Per-folder music commands | Auto-generated for each folder of MP3s on the SD card — play the Nth (or first/random, if shuffle is on) track in that folder |

While a track is playing, the screen shows a live **now-playing** view: song
name, folder, progress bar, elapsed/total time, volume, shuffle/repeat
state, and the visualizer's on/off state and style number.

### Bluetooth pairing

| Command | Effect |
|---|---|
| `pair` | Scan for and pair with a Bluetooth speaker/headset |
| `btstatus` | Report current Bluetooth connection status |
| `btdisconnect` | Disconnect the current device |
| `btforget` | Forget the saved device and stop auto-reconnect |

---

## LED music visualizer

The 10 SK6812 LEDs on the Core2's side faces (GPIO 25) render one of **100
audio-reactive styles** while music plays — VU bars, spectrum analyzers,
chases, comets, sparkles, and more. See
[docs/CORE2_VISUALIZER.md](docs/CORE2_VISUALIZER.md) for the full style list
and implementation details.

| Command | Effect |
|---|---|
| `visualizer` | Toggle on/off, or pass a style number 1-100 to select + enable |
| `visualizernext` / `visualizerprevious` | Cycle styles (wraps 1↔100), enables the visualizer |
| `ledcolor` | Set all 10 LEDs to a solid color (or `off`) — independent of the visualizer |

On/off state and selected style persist across reboots. Default style is
**1 — VU bars**.

---

## AI voice assistant & vision

Requires an OpenAI API key, set via the
[SD card config file](#sd-card-configuration) (`openai_key=sk-...`) or the
device's web configuration page.

| Command | Effect |
|---|---|
| `listen` | Record 4 seconds of audio from the active microphone, transcribe it with Whisper, and send the result as an AI prompt — for example, "show me a picture of a cat" will look one up and display it |
| `askgpt` | Ask a general question (no picture context); the reply is returned (and optionally spoken) |
| `askpic` | Ask a question about the currently displayed picture using AI vision |
| `speak` | Speak arbitrary text aloud via TTS |
| `aitts` | Toggle whether `askpic`/`askgpt`/`listen` replies are spoken aloud (default on) |
| `micsrc` | Switch the microphone source between the built-in PDM mic (`pdm`, default) and an external Grove analog mic (`grove`) — see [Core2 Grove Microphone Adapter](Core2-Grove-Mic-Adapter.md) |

---

## Power management

| Command | Effect |
|---|---|
| `battery` | Report battery level (0-100%), voltage (mV), and charging status (AXP192) |
| `sleeptimer` | Set minutes of inactivity before deep sleep (0 = never) |
| `sleeponpower` | Toggle whether the sleep timer also applies on USB power (default: only sleeps on battery) |
| `sleep` | Enter deep sleep immediately; wake by touching the screen |

---

## Wi-Fi provisioning

On first boot, the Core2 starts a SoftAP named **`TCMD-Core2-XXXXXX`**:

1. Connect your phone or laptop to that network.
2. Browse to **`http://192.168.4.1`** and enter your Wi-Fi SSID and password.
3. Click **Save & Connect** — the device restarts and joins your network.

Alternatively, place a [`config.txt`](#sd-card-configuration) file on the SD
card before first boot to skip the SoftAP flow entirely.

---

## SD card configuration

On boot, the firmware checks for **`config.txt`** in the root of the SD card
and writes any credentials it contains to NVS (overwriting existing values)
before connecting to Wi-Fi:

```
# Wi-Fi networks (up to three)
ssid=MyNetwork
password=mypassword
ssid2=BackupNetwork
password2=backuppass
ssid3=ThirdNetwork
password3=thirdpass

# OpenAI API key (used for listen/askpic/askgpt/speak)
openai_key=sk-proj-...

# Optional: keep secrets on the SD card only (do not copy them to NVS)
# secrets_in_sd=1
```

#### Default behaviour (`secrets_in_sd` absent or `=0`)

Secrets are **moved** to NVS on first boot:

1. Each secret key is written to NVS.
2. If all writes succeed, the secret lines are removed from `config.txt` so
   the file no longer contains plaintext credentials.
3. The device reconnects on every subsequent boot using NVS alone — the SD
   card is not required.

If any NVS write fails, the secrets are left in `config.txt` intact so they
can be retried on the next boot.

#### `secrets_in_sd=1` — SD-only mode

Secrets are **never written to NVS** and remain in `config.txt`:

- WiFi credentials and the OpenAI key are loaded into RAM only for the
  current boot, read fresh from the card each time.
- If the SD card is absent at boot, the device falls back to whatever NVS
  already contains (which may be empty, triggering SoftAP provisioning).
- Existing NVS credentials are not erased — the setting only suppresses new
  writes.

Use this when you want physical control over access: pulling the SD card makes
the credentials unavailable to anyone who reflashes the device.

### Settings file — `core2_settings.cfg`

The `save` command writes a second file, **`/sdcard/core2_settings.cfg`**,
that stores the current display state:

```
bg=0,0,0
fg=255,255,255
orient=0
font=2
mp3=0
jpeg=https://loremflickr.com/320/240/cat
text=Hello\nWorld
```

| Key | Values | Meaning |
|-----|--------|---------|
| `bg` | `R,G,B` (0-255) | Background color |
| `fg` | `R,G,B` (0-255) | Text color |
| `orient` | `0` / `1` | Portrait / Landscape |
| `font` | `1`–`8` | Font scale |
| `mp3` | `0` / `1` | Whether music mode was active at save time |
| `jpeg` | URL or `/sdcard/…` path | Last displayed image (empty if none) |
| `text` | string (newlines as `\n`) | Last displayed text (empty if none) |

On boot the firmware tries this file before NVS, so settings here survive a
firmware reflash. You can also hand-edit the file between reboots — for
example to pre-configure the background color or starting text on a new
device using the same SD card.

After a successful `save`, the result screen shows all saved values and
confirms where they were written ("SD card + NVS" or "NVS only").

---

## Hardware notes

| Item | Detail |
|------|--------|
| Board | M5Stack Core2 for AWS |
| Display | ILI9342C SPI TFT, 320×240 (landscape native) |
| PMU | AXP192 (I2C) — LCD power/backlight, battery gauge |
| Touch | FT6336U capacitive, I2C 0x38 (shared bus) |
| LEDs | 10x SK6812, GPIO 25 (side bars) |
| Audio | Bluetooth A2DP output, onboard microphone |
| Console | UART0 via CP2104 USB-UART bridge |
| Memory | QSPI PSRAM (8 MB) |

---

## Building this variant

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant core2
```

Output firmware path: `docs/firmware/esp32_core2_picture_frame.bin`

## Related files

- `main/picture_frame.c` - main firmware flow for this variant
- `main/core2_leds.c` - SK6812 visualizer LED driver
- `main/screen_control_core2.c` - ILI9342C + AXP192 display driver
- `main/picture_frame_commands.json` - TRIGGERcmd command reference (documentation only)
- `sdkconfig.core2` - variant configuration
- `docs/manifest-core2.json` - browser installer manifest
- [docs/CORE2_VISUALIZER.md](docs/CORE2_VISUALIZER.md) - full visualizer style reference
- [Core2-Grove-Mic-Adapter.md](Core2-Grove-Mic-Adapter.md) - wiring and setup for an external TRRS headset mic via Grove Port B
