# M5Stack ATOM Echo - TRIGGERcmd Voice Variant

This variant targets the **M5Stack ATOM Echo** (ESP32-PICO-D4) and uses
TRIGGERcmd cloud APIs for pairing, health checks, and voice-triggered commands.
Unlike the SSH LED and SSH screen variants, this firmware is button and
voice driven.

---

## What it does

1. Connects to Wi-Fi (or enters BLE Improv provisioning if credentials are not present).
2. Fetches and speaks a TRIGGERcmd pairing code through the onboard speaker.
3. Waits in idle mode for button input.
4. On short press, runs an authenticated TRIGGERcmd API health check.
5. On long press, records audio, performs speech-to-text, and sends the text to TRIGGERcmd Chat API.

---

## Button actions

| Button action | Behavior |
|---------------|----------|
| Short press (< 1 s) | Health check API call; LED/beep indicates success or failure |
| Long press (>= 1 s) | Record voice until release (max 10 s), then transcribe and submit |

---

## LED status indicators

| Color | Meaning |
|-------|---------|
| Yellow | Boot/init |
| Blue blinking | Wi-Fi provisioning / connecting |
| Green (brief) | Wi-Fi connected |
| White | Waiting for pairing |
| Dim purple | Idle, ready |
| Cyan | Recording |
| Yellow | Processing STT/API |
| Green (brief) | Operation success |
| Red (brief) | Operation failed |
| Solid red | Fatal error |

---

## Hardware notes

| Signal | GPIO |
|--------|------|
| Button | 39 |
| SK6812 RGB LED | 27 |
| Speaker BCK | 19 |
| Speaker LRCK | 33 |
| Speaker DOUT | 22 |
| PDM Mic CLK | 33 |
| PDM Mic DATA | 23 |

Important: GPIO 33 is shared between speaker LRCK and microphone CLK. The
firmware explicitly deinitializes/reinitializes audio paths when switching
between playback and recording.

---

## Audio behavior

- Pair code speech uses embedded PCM clips (one clip per character).
- Success beep: ascending two-tone.
- Failure beep: descending two-tone.
- Recording path captures mono PDM audio and wraps it as WAV for STT upload.

---

## Building this variant

In `menuconfig`, under **ESP32 SSH LED Configuration**, choose:

> **Hardware variant -> TRIGGERcmd ATOM Echo (M5Stack ATOM Echo - ESP32-PICO-D4)**

Then build using the project build script:

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant tcmd_atom_echo
```

Output firmware path:

- `docs/firmware/esp32_tcmd_atom_echo.bin`

---

## Related docs

- `docs/TCMD_ATOM_ECHO.md` for full implementation details and architecture.
- `docs/manifest-tcmd_atom_echo.json` for the browser installer manifest.
