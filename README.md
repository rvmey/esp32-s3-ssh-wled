# ESP32-S3 RGB LED Controller via SSH

Control the onboard WS2812 RGB LED (GPIO 48) on the **ESP32-S3-DevKitC-1** over
a real SSH connection powered by **wolfSSH**.

---

## Project layout

```
esp32_ssh_led/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml      ← declares wolfSSH + led_strip dependencies
│   ├── Kconfig.projbuild      ← WiFi / SSH credentials (menuconfig)
│   ├── main.c
│   ├── wifi_manager.{h,c}
│   ├── led_control.{h,c}      ← WS2812 driver (RMT, GPIO 48)
│   └── ssh_server.{h,c}       ← wolfSSH server + shell
```

---

## Prerequisites

| Tool | Version |
|------|---------|
| ESP-IDF | 5.1 or later |
| Python | 3.8+ (bundled with IDF) |
| wolfSSH component | fetched automatically by IDF Component Manager |

---

## Build & flash

### 1  Configure credentials

```bash
idf.py menuconfig
```

Navigate to **"ESP32 SSH LED Configuration"** and set:

| Setting | Default | Notes |
|---------|---------|-------|
| WiFi SSID | `MyWifi` | Your 2.4 GHz network name |
| WiFi Password | *(empty)* | Leave empty for open networks |
| SSH Port | `22` | Change to e.g. `2222` if port 22 is blocked |
| SSH Username | `admin` | Login name |
| SSH Password | `esp32led` | Login password — **change this!** |

### 2  Build

```bash
idf.py build
```

The IDF Component Manager will automatically download **wolfSSH**, **wolfSSL**,
and the **espressif/led_strip** component on the first build.

### 3  Flash & monitor

```bash
idf.py -p COM<N> flash monitor
```

The serial monitor will print the device's IP address once Wi-Fi connects.

---

## Connecting

```bash
ssh admin@<device-ip>
# or on a non-standard port:
ssh admin@<device-ip> -p 2222
```

Use the password set in menuconfig (default: `esp32led`).

> **Host key fingerprint** — On first boot the device generates and persists an
> ECC P-256 host key in NVS.  Your SSH client will ask you to accept it once.
> To reset the key, erase NVS with `idf.py erase-flash`.

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
| Brief blue | LED driver initialised |
| Brief green | Wi-Fi connected, IP obtained |
| Solid red | Error (Wi-Fi failed or SSH init failed) — check serial log |

---

## Security notes

* The default password `esp32led` is intentionally obvious — set a strong
  password via `menuconfig` before deploying.
* wolfSSH uses the wolfSSL cryptographic library which implements standard
  SSH-2 with AES, ECC, and SHA-2; the connection is fully encrypted.
* The host key is stored in NVS (unencrypted by default). Enable NVS
  encryption (`CONFIG_NVS_ENCRYPTION=y`) for extra protection.
* Only **password authentication** is enabled. Public-key auth can be added
  by extending `user_auth_cb` in `ssh_server.c`.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| `wolfSSH_init failed` | wolfSSL not configured — check `idf_component.yml` version |
| Host key error at connect | NVS corrupt — run `idf.py erase-flash` |
| `bind() failed on port 22` | Some routers block port 22 from LAN — try port 2222 |
| Wrong LED colour / no output | Wrong pixel format — change `LED_PIXEL_FORMAT_GRB` ↔ `LED_PIXEL_FORMAT_RGB` in `led_control.c` |
