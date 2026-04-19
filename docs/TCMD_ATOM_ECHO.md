# TRIGGERcmd ATOM Echo Firmware Variant

A new ESP-IDF firmware variant (`tcmd_atom_echo`) for the **M5Stack ATOM Echo** smart speaker. It connects to Wi-Fi, fetches a TRIGGERcmd pair code and speaks it aloud via the built-in speaker, then idles with a dim LED. A **short button press** triggers an authenticated TRIGGERcmd health check. A **long press** starts voice recording via the PDM microphone; on release the audio is transcribed via an STT service and sent to the TRIGGERcmd Chat API.

---

## Hardware

**M5Stack ATOM Echo** — SKU C008-C

| Feature | Detail |
|---------|--------|
| SoC | ESP32-PICO-D4 (240 MHz dual-core, **classic ESP32**, 4 MB flash) |
| Button | GPIO 39 — active-low, internal pull-up |
| RGB LED | SK6812 single pixel — GPIO 27 (RMT) |
| I²S Speaker amp | NS4168 — BCK=GPIO19, LRCK=GPIO33, DOUT=GPIO22 |
| PDM Microphone | SPM1423 PDM — CLK=GPIO33 (shared with speaker LRCK), DATA=GPIO23 |
| GROVE port | G26, G32 (not used) |

> **Note:** This is a **classic ESP32**, not ESP32-S3. The sdkconfig target is `esp32`. The USB console uses an FTDI UART bridge (no USB-JTAG).

---

## Firmware Boot Flow

```
Power on
  │
  ├─ atom_led_init() + atom_audio_init()
  │    LED = yellow
  │
  ├─ Wi-Fi credentials in NVS?
  │    No  → BLE Improv Wi-Fi provisioning (LED = slow blue blink)
  │    Yes ─┐
  │         ▼
  ├─ wifi_connect()
  │    Fail → LED red + fail beep → suspend forever
  │    OK   → LED green 1 s → off
  │
  ├─ NVS namespace "ae_cfg": read hw_token
  │
  ├─ hw_token present?
  │    No  → PAIR CODE LOOP ──────────────────────────────────────────────────┐
  │          │                                                                 │
          ├─ GET /pair/index                                                  │
          │    → { pairCode, pairToken }                                    │
          │                                                                 │
          ├─ LED = white (waiting for pairing)                              │
          │                                                                 │
          ├─ Speak pair code (audio clips, one character at a time)         │
          │                                                                 │
          ├─ Poll GET /pair/lookup (Bearer pairToken) every 5 s             │
  │          │    On button press while waiting → speak pair code again        │
  │          │    Every 30 s → speak pair code again                           │
  │          │    Timeout (10 min / 120 polls) → fetch new pair code, restart  │
  │          │    Authorized → save hw_token to NVS ──────────────────────────┘
  │          │
  └─ IDLE LOOP
       LED = dim purple
       Short press (<1 s)?
         ├─ LED = white flash
         ├─ GET /api/v1/chat/conversations  (Bearer token — Chat API health check)
         │    HTTP 200 → LED green 2 s + beep_ok()
         │    Failure  → LED red 2 s + beep_fail()
         └─ Return to dim purple
       Long press (≥1 s)?
         ├─ LED = cyan (recording)
         ├─ Capture PDM audio until button released (max 10 s)
         ├─ LED = yellow (processing)
         ├─ POST audio WAV to STT service → transcript text
         │    STT failure → LED red 2 s + beep_fail() → return to idle
         ├─ POST { message: transcript, conversationId } to /api/v1/chat/message
         │    Save returned conversationId to NVS
         │    HTTP 200 → LED green 2 s + beep_ok()
         │    Failure  → LED red 2 s + beep_fail()
         └─ Return to dim purple
```

---

## New Files

| File | Purpose |
|------|---------|
| `main/tcmd_atom_echo.c` | Variant main loop: Wi-Fi, pair code, health check, voice query |
| `main/tcmd_atom_echo.h` | Public `tcmd_atom_echo_run()` declaration |
| `main/atom_led.c` | SK6812 single-pixel LED via RMT (`espressif/led_strip`) |
| `main/atom_led.h` | `atom_led_init()`, `atom_led_set(r, g, b)` |
| `main/atom_audio.c` | I²S TX init (NS4168), PCM playback, programmatic beep tones |
| `main/atom_audio.h` | `atom_audio_init()`, `atom_audio_play_clip()`, `atom_audio_beep_ok()`, `atom_audio_beep_fail()` |
| `main/atom_mic.c` | PDM microphone capture via I²S RX (SPM1423), WAV file assembly |
| `main/atom_mic.h` | `atom_mic_init()`, `atom_mic_record(buf, max_ms)` → returns WAV byte length |
| `main/audio_clips.c` | Embedded 8 kHz 16-bit mono PCM arrays for each character |
| `main/audio_clips.h` | `extern` declarations + `clip_for_char()` lookup |
| `sdkconfig.tcmd_atom_echo` | ESP32 (classic) sdkconfig for this variant |
| `docs/manifest-tcmd_atom_echo.json` | Web-flash manifest |

---

## Modified Files

| File | Change |
|------|--------|
| `main/Kconfig.projbuild` | Add `HARDWARE_TCMD_ATOM_ECHO` choice entry |
| `main/CMakeLists.txt` | Add `elseif(CONFIG_HARDWARE_TCMD_ATOM_ECHO)` branch (no SSH_SRCS) |
| `main/main.c` | Add `#elif CONFIG_HARDWARE_TCMD_ATOM_ECHO` stubs; bump `APP_VERSION` |
| `main/improv_wifi.c` | Bump firmware version string |
| `build.ps1` | Add `tcmd_atom_echo` entry to `$variants` array |

---

## Audio Clips

The pair code is spoken aloud one character at a time using pre-recorded PCM clips embedded in flash.

**Format:** 8 kHz, 16-bit signed, mono (approx. 0.4 s per clip → ~6 KB each → ~230 KB total for all 36)

**Generation workflow (developer task):**

The audio clips are in atom-echo/char-clips

**Required clips — 36 characters:**

| Category | Characters |
|----------|-----------|
| Digits (10) | `0` `1` `2` `3` `4` `5` `6` `7` `8` `9` |
| Letters (26) | `A` `B` `C` `D` `E` `F` `G` `H` `I` `J` `K` `L` `M` `N` `O` `P` `Q` `R` `S` `T` `U` `V` `W` `X` `Y` `Z` |

**Audio beeps (programmatically generated — no clip needed):**

| Event | Pattern |
|-------|---------|
| `beep_ok()` | 440 Hz → 880 Hz ascending two-tone, ~300 ms each |
| `beep_fail()` | 880 Hz → 220 Hz descending two-tone, ~300 ms each |

---

## I²S Configuration (NS4168 speaker amp)

**Important:** GPIO33 is shared with the PDM microphone CLK. The firmware uses a runtime handoff:
- Idle / pair-code / beeps: speaker owns GPIO33 as I2S1 LRCK
- Recording: speaker I2S is deinitialized, mic owns GPIO33 as I2S0 PDM CLK
- After recording: mic deinitialized, speaker I2S reinitialized

```c
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
chan_cfg.auto_clear = true;

i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),  /* 16 kHz */
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
      .bclk = 19,                   /* BCK */
      .ws   = 33,                   /* LRCK (shared pin, speaker ownership phase) */
        .dout = 22,                   /* DOUT */
        .din  = I2S_GPIO_UNUSED,
    },
};
```

---

## PDM Microphone Configuration (SPM1423)

The SPM1423 is driven via the ESP32 I²S peripheral in PDM RX mode on **I2S_NUM_0**. GPIO33 is shared between the microphone CLK (I2S0 PDM) and the speaker LRCK (I2S1 STD), but GPIO33 remains routed to the microphone's PDM clock permanently — the speaker uses GPIO19 (BCK) for timing and does not need the LRCK pin to toggle independently.

**New IDF v6.0 PDM driver:**

```c
i2s_pdm_rx_config_t pdm_cfg = {
    .clk_cfg = {
        .sample_rate_hz = 16000,
        .clk_src        = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        .dn_sample_mode = I2S_PDM_DSR_16S,   /* bclk = 16000 × 128 = 2.048 MHz */
        .bclk_div       = 8,
    },
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
        .clk  = 33,   /* PDM CLK  (shared with speaker LRCK) */
        .din  = 23,   /* PDM DATA */
    },
};
pdm_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;  /* SPM1423 SEL=VDD → right channel */
```

`atom_mic_record()` reads DMA buffers until the button is released (max 10 s), assembles a standard 44-byte WAV header, and returns the heap-allocated buffer. The caller (voice-query handler) POSTs it directly to the STT service and then frees it.

---

```ini
CONFIG_IDF_TARGET="esp32"                          # Classic ESP32, not S3
CONFIG_HARDWARE_TCMD_ATOM_ECHO=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y                  # UART0 via FTDI (no USB-JTAG)
CONFIG_BT_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y                   # BLE only — for Improv Wi-Fi provisioning
CONFIG_BLUEDROID_ENABLED=y
CONFIG_FLASH_SIZE_4MB=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y                # Full CA bundle for TLS
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y
```

> No SPIRAM/OPI PSRAM, no USB-JTAG, no SSH server, no Socket.IO in this variant.

---

## build.ps1 Entry

```powershell
[PSCustomObject]@{
    Name      = 'tcmd_atom_echo'
    Config    = 'sdkconfig.tcmd_atom_echo'
    BuildDir  = 'build_tcmd_atom_echo'
    OutputBin = 'esp32_tcmd_atom_echo.bin'
}
```

**Build command:**

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null
.\build.ps1 -Variant tcmd_atom_echo
# Output: docs/firmware/esp32_tcmd_atom_echo.bin
```

---

## NVS Layout

Namespace: **`ae_cfg`** (distinct from `pf_cfg` used by picture_frame, `ssh_cfg` used by SSH variants)

| Key | Type | Max size | Contents |
|-----|------|----------|---------|
| `hw_token` | string | 512 B | TRIGGERcmd user JWT (Bearer token) |
| `conversation_id` | string | 64 B | Active Chat API conversation ID (persisted across reboots) |
| `stt_key` | string | 128 B | STT service API key (provisioned via config HTTP server at first boot) |


---

## TRIGGERcmd API Calls

| Endpoint | Method | Auth | Purpose |
|----------|--------|------|---------|
| `/pair/index` | GET | None | Obtain pair code + pair token |
| `/pair/lookup` | GET | Bearer `pairToken` | Poll for user authorization |
| `/api/v1/chat/conversations` | GET | Bearer `userToken` | Health check — HTTP 200 = healthy |
| `/api/v1/chat/message` | POST JSON | Bearer `userToken` | Send voice transcript; receive AI response |

> **STT service** (external, not TRIGGERcmd): The device POSTs a 16 kHz 16-bit mono WAV to an HTTP STT endpoint (e.g., Google Speech-to-Text REST API, OpenAI Whisper API). The service URL and API key are stored in NVS as `stt_key` and must be provisioned by the user at first boot via the device's config HTTP server.

> **Chat API response**: `assistantMessage.content` is received as text but not synthesized to speech in this variant — the device signals success/failure with LED colour and beep only. The full response text is logged to UART for diagnostic purposes.

---

## Implementation Phases

### Phase 1 — Scaffolding
- Add `HARDWARE_TCMD_ATOM_ECHO` to `Kconfig.projbuild`
- Add variant branch in `CMakeLists.txt` (no SSH_SRCS)
- Add `#elif CONFIG_HARDWARE_TCMD_ATOM_ECHO` stubs in `main.c` + version bump 2.0.45 → 2.0.46
- Add variant to `build.ps1`
- Create `docs/manifest-tcmd_atom_echo.json`

### Phase 2 — HAL Drivers
- `atom_led.c/.h` — RMT SK6812 via `espressif/led_strip` (same component as `led_control.c`)
- `atom_audio.c/.h` — I²S TX init (I2S_NUM_1, BCK=GPIO19, LRCK=GPIO33, DOUT=GPIO22) + PCM clip playback + sine-wave beep generation
- `atom_mic.c/.h` — I²S RX PDM input (SPM1423, CLK=GPIO33, DATA=GPIO23, 16 kHz 16-bit mono); `atom_mic_record()` captures until button release (max 10 s) and returns a heap-allocated WAV buffer with standard 44-byte header (caller frees)
- `audio_clips.c/.h` — stub placeholder arrays (empty); replaced with real clips after generation script runs

### Phase 3 — Variant Logic
- `tcmd_atom_echo.c/.h` implements `tcmd_atom_echo_run()`:
  - Wi-Fi provisioning (reuses `wifi_manager.c` + `improv_wifi.c`)
  - Config HTTP server at first boot if `stt_key` not in NVS (user browses to device IP to enter STT API key)
  - HTTPS helpers (same pattern as `picture_frame.c`: `https_get_simple`, `https_get_auth`, `https_post_json`, `json_extract_str`)
  - Pair code loop: `GET /pair/index` → speak code → poll `GET /pair/lookup` (mirrors `picture_frame_run()` lines ~808–841)
  - Short-press ISR → health check (`GET /api/v1/chat/conversations`)
  - Long-press ISR → voice query: `atom_mic_record()` → HTTP POST WAV to STT service → `POST /api/v1/chat/message` with transcript + `conversation_id` → save new `conversation_id` to NVS

### Phase 4 — sdkconfig
- Create `sdkconfig.tcmd_atom_echo` with classic ESP32 target + BLE + no PSRAM

### Phase 5 — Audio Clip Generation (developer task)
- Run generation script for all 36 characters (see table above)
- Convert to 8 kHz 16-bit mono raw PCM via ffmpeg
- Embed as C arrays in `audio_clips.c`

### Phase 6 — Build & Verify
1. `.\.build.ps1 -Variant tcmd_atom_echo` — must compile without errors on ESP32 target
2. Flash to ATOM Echo; BLE appears for Improv Wi-Fi provisioning
3. After Wi-Fi: config HTTP server starts if no `stt_key` stored — browse to device IP, enter STT API key
4. Pair code spoken aloud, LED white; enter code at triggercmd.com → LED goes dim purple
5. Short button press:
   - Chat API reachable (HTTP 200): LED green 2 s + ascending beep
   - Unreachable / invalid token: LED red 2 s + descending beep
6. Long button press (hold ≥ 1 s, speak, release):
   - LED cyan during recording, yellow during processing
   - STT returns transcript → transcript POSTed to Chat API
   - LED green + ascending beep on success; LED red + descending beep on failure
   - UART log shows `assistantMessage.content`
