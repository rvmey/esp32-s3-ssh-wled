# M5Stack AtomS3 Lite - TRIGGERcmd RGB LED

This variant targets the **M5Stack AtomS3 Lite** (ESP32-S3) and uses
TRIGGERcmd cloud APIs to control the onboard RGB LED in real time.

It does not use the SSH command shell. Instead it connects to TRIGGERcmd,
receives commands over Socket.IO, and sets the LED color immediately.

---

## What it does

1. Connects to Wi-Fi (or enters SoftAP provisioning if credentials are absent).
2. Starts an HTTP config server on port 80 (device IP).
3. Pairs to TRIGGERcmd with a pair code flow (`/pair` + `/pair/lookup`).
4. Creates/registers a computer identity (`TCMDATOMS3-<MAC>`).
5. Syncs the `color`, `off`, and `reboot` commands to TRIGGERcmd.
6. Connects to Socket.IO and subscribes to its TRIGGERcmd room.
7. Executes incoming commands and reports completion (`/api/run/save`).

---

## Supported TRIGGERcmd commands

| Command | Parameters | Effect |
|---------|------------|--------|
| `color` | named color or hex (`#RRGGBB`) | Set the RGB LED color |
| `off` | none | Turn the LED off |
| `reboot` | none | Restart the device |

Named color shortcuts: `red`, `green`, `blue`, `white`, `black`, `yellow`,
`cyan`, `magenta`, `orange`, `purple`, `pink`, `gray`.

The last set color is saved to NVS and restored on reboot.

---

## Button

| Press type | Behavior |
|------------|----------|
| Single click | Triggers the configured Core2 command **or** calls the Single-Click Bookmark URL |
| Double click | Triggers the configured Core2 command **or** calls the Double-Click Bookmark URL |
| Triple click | Triggers the configured Core2 command **or** calls the Triple-Click Bookmark URL |
| Long press (≥ 2 s) | Triggers the configured Core2 command **or** calls the Long-Press Bookmark URL |

Each button action has two independent slots:
- **Core2 command** — if configured, the button sends that command to the Core2 picture frame (see [Core2 Remote Trigger](#core2-remote-trigger) below). The bookmark URL is ignored for that button.
- **Bookmark URL** — if no Core2 command is configured, the button calls this HTTP GET endpoint as before (home-automation scene, webhook, IFTTT applet, etc.).

Single/double/triple clicks are distinguished by a 350 ms window after the last tap; long press fires immediately while the button is still held.

---

## Core2 Remote Trigger

The AtomS3 Lite can control a **M5Stack Core2** picture frame on the same Wi-Fi network by sending commands directly over HTTP — no cloud required. TRIGGERcmd cloud is used as a fallback if the local network path fails.

### How it works

1. **Auto-discovery** — on boot, the AtomS3 Lite broadcasts a UDP discovery packet on port 5380. The Core2 replies with its IP address. No manual configuration of the Core2's IP is needed.
2. **Local HTTP trigger** — button press sends `POST http://<core2-ip>/trigger` with a JSON body `{"trigger":"<command>","params":"<parameters>"}`. Response arrives in under a second.
3. **Rediscovery on failure** — if the local POST fails (Core2 rebooted, got a new IP), the AtomS3 Lite re-runs UDP discovery and retries once before falling back to the cloud.
4. **Cloud fallback** — if both local attempts fail and a Core2 computer name is configured, the command is sent via `POST https://www.triggercmd.com/api/run/triggerSave` using the same TRIGGERcmd account token. This requires internet but works across networks.

### Configuration

Configure the Core2 trigger on the **SoftAP provisioning page** (`http://192.168.4.1` during first-boot setup) or on the **post-Wi-Fi config page** (`http://<device-ip>/`):

| Field | Purpose |
|-------|---------|
| **Core2 IP** | Manual IP override. Leave blank to use UDP auto-discovery. |
| **Core2 computer name** | The TRIGGERcmd computer name for the Core2 (e.g. `TCMDCORE2-A4F00FDFA518`) — used only for cloud fallback. |
| **Single-click command / params** | Core2 command name and optional parameters for single click. |
| **Double-click command / params** | Core2 command name and optional parameters for double click. |
| **Triple-click command / params** | Core2 command name and optional parameters for triple click. |
| **Long-press command / params** | Core2 command name and optional parameters for long press. |

Leave both the command and the bookmark URL blank to make a button do nothing.

### Example commands

Any command the Core2 supports via TRIGGERcmd can be used. Common examples:

| Command | Params | Effect |
|---------|--------|--------|
| `speak` | `Hello from AtomS3` | Core2 speaks the text aloud |
| `text` | `Hello!` | Display text on the Core2 screen |
| `color` | `blue` | Set Core2 background color |
| `jpeg` | `https://example.com/pic.jpg` | Display a picture on the Core2 |
| `play` | *(empty)* | Resume MP3 playback on Core2 |
| `pause` | *(empty)* | Pause MP3 playback |
| `clock` | `digital` | Show the clock on Core2 |
| `visualizer` | `1` | Enable LED visualizer style 1 |

See [Core2-TRIGGERcmd-Display.md](Core2-TRIGGERcmd-Display.md) for the full command reference.

---

## LED status indicators

| Color | Meaning |
|-------|---------|
| Yellow | Boot / init |
| Blue blinking | Wi-Fi SoftAP provisioning |
| Green (brief) | Wi-Fi connected |
| Dim white | Waiting for TRIGGERcmd pair code authorisation |
| Dim blue | Connecting to Socket.IO |
| Dim purple | Idle, paired and connected |
| White flash | Bookmark URL being called |
| Green flash | Bookmark URL call succeeded |
| Red flash | Bookmark URL call failed |
| Blue flash | Core2 command being sent (local or cloud) |
| Green flash | Core2 command delivered successfully |
| Red flash | Core2 command failed (local + cloud both failed) |
| Solid red | Fatal error (Wi-Fi failed) |

---

## Wi-Fi provisioning

The AtomS3 Lite uses **SoftAP provisioning** (no USB-JTAG Improv needed):

1. On first boot, the device starts a Wi-Fi access point named **`TCMD-AtomS3-XXXXXX`**.
2. Connect your phone or laptop to that network.
3. Browse to **`http://192.168.4.1`** and fill in your Wi-Fi SSID, password, and
   optionally up to four Bookmark URLs (single/double/triple click, long press), and up to two secondary networks.
4. Click **Save & Connect** — the device restarts and joins your network.

---

## HTTP config page

Once connected, browse to the device's IP address (`http://<device-ip>/`) to:

- See the TRIGGERcmd pair code (during initial pairing).
- Update the Single-Click, Double-Click, Triple-Click, and Long-Press Bookmark URLs at any time.
- Add or change secondary Wi-Fi networks.
- Re-provision the device to a different TRIGGERcmd account.

---

## Hardware notes

| Signal | GPIO |
|--------|------|
| WS2812C RGB LED | 35 |
| User button | 41 (active-low, internal pull-up) |
| Console | USB Serial/JTAG (built-in ESP32-S3) |

---

## Building this variant

In `menuconfig`, under **ESP32 SSH LED Configuration**, select:

> **Hardware variant → TRIGGERcmd AtomS3 Lite (M5Stack AtomS3 Lite, ESP32-S3, RGB LED + button)**

Build with the project script:

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant atoms3_lite
```

Output firmware path:

- `docs/firmware/esp32_atoms3_lite.bin`

---

## Related files

- `main/tcmd_atoms3_lite.c` — main firmware flow for this variant
- `main/atoms3_led.c` — WS2812C LED driver (GPIO 35)
- `sdkconfig.atoms3_lite` — variant configuration
- `docs/manifest-atoms3_lite.json` — browser installer manifest
