# ESP32-S3 SSH Server

Control an ESP32-S3 output device (RGB LED or colour screen) over a real SSH
connection powered by **[wolfSSH](https://www.wolfssl.com/wolfssh/)**.  Two
hardware variants are supported; choose the one that matches your board.

| Variant | Hardware | What it controls |
|---------|----------|------------------|
| [ESP32-S3-DevKitC-1](ESP32-S3-DevKitC-1.md) | DevKitC-1 | Onboard WS2812 RGB LED (GPIO 48) |
| [JC3248W535](JC3248W535.md) | Guition JC3248W535 | 320×480 AXS15231 QSPI display |

---

## Browser installer

The easiest way to flash the firmware is the **web installer** — no toolchain
required, just a Chromium-based browser (Chrome, Edge, Opera):

**<https://rvmey.github.io/esp32-s3-ssh-wled/>**

1. Open the page, select your hardware variant.
2. Click **Install** and choose the serial port.
3. Enter your WiFi credentials when prompted — they go directly to the device
   over USB and are never sent to any server.
4. Connect via SSH once the device reports its IP address.

---

## Prerequisites

| Tool | Version |
|------|---------|
| ESP-IDF | 5.1 or later |
| Python | 3.8+ (bundled with IDF) |
| wolfSSH component | fetched automatically by IDF Component Manager |

---

## Build & flash

### 1  Configure credentials and hardware variant

```bash
idf.py menuconfig
```

Navigate to **"ESP32 SSH LED Configuration"** and set:

| Setting | Default | Notes |
|---------|---------|-------|
| **Hardware variant** | `DevKitC-1` | Select your target board |
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

> **ESP-IDF v6.0 note** — wolfssl 5.8.2 has minor incompatibilities with
> ESP-IDF v6.0 (C23 keyword conflict, removed peripheral module defines).
> Run `.\apply-patches.ps1` once after the first build (which populates
> `managed_components/`) to apply the fixes.  `build.ps1` does this
> automatically.

### 3  Flash & monitor

```bash
idf.py -p COM<N> flash monitor
```

The serial monitor prints the device's IP address once Wi-Fi connects.

---

## Connecting

```bash
ssh admin@<device-ip>
```

Use the password set in menuconfig (default: `esp32led`).

> **Host key fingerprint** — On first boot the device generates and persists an
> ECC P-256 host key in NVS.  Your SSH client will ask you to accept it once.
> To reset the key, erase NVS with `idf.py erase-flash`.

---

## Security notes

* wolfSSH uses the wolfSSL cryptographic library which implements standard
  SSH-2 with AES, ECC, and SHA-2; the connection is fully encrypted.
* The host key is stored in NVS (unencrypted by default). Enable NVS
  encryption (`CONFIG_NVS_ENCRYPTION=y`) for extra protection.