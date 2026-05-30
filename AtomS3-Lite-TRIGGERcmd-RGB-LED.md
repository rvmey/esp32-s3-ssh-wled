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
| Short press (< 2 s) | Calls the configured Short Bookmark URL (HTTP GET, no TLS cert check) |
| Long press (≥ 2 s) | Calls the configured Long Bookmark URL (HTTP GET, no TLS cert check) |

Both Bookmark URLs let you trigger any HTTP endpoint — a home-automation scene,
a webhook, an IFTTT applet, etc. — directly from the physical button.

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
| Solid red | Fatal error (Wi-Fi failed) |

---

## Wi-Fi provisioning

The AtomS3 Lite uses **SoftAP provisioning** (no USB-JTAG Improv needed):

1. On first boot, the device starts a Wi-Fi access point named **`TCMD-AtomS3-XXXXXX`**.
2. Connect your phone or laptop to that network.
3. Browse to **`http://192.168.4.1`** and fill in your Wi-Fi SSID, password, and
   optionally a **Short Bookmark URL**, a **Long Bookmark URL**, and up to two secondary networks.
4. Click **Save & Connect** — the device restarts and joins your network.

---

## HTTP config page

Once connected, browse to the device's IP address (`http://<device-ip>/`) to:

- See the TRIGGERcmd pair code (during initial pairing).
- Update the Short and Long Bookmark URLs at any time.
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
