# ESP32-S3 Bike Tracker

This variant targets the **ESP32-S3-DevKitC-1** (8 MB flash) and turns it into
a GPS bike-ride data logger.  The device spends virtually all its time in deep
sleep (~10 µA), wakes on motion detected by an MPU6050 accelerometer, records
GPS track-points to a dedicated 5.9 MB NVS partition, and uploads completed
rides as JSON over WiFi at the end of each ride.

---

## How it works

1. **First boot** — Improv-WiFi provisioning runs so you can set your SSID,
   password and upload URL via the web installer.  The device then arms the
   MPU6050 motion interrupt and enters deep sleep.

2. **Motion detected** — The MPU6050 INT pin goes high, triggering an EXT0
   wakeup.  The firmware acquires a GPS fix (up to 60 s), then logs a
   track-point (lat / lon / speed) every 5 seconds.

3. **Ride ends** — After 30 s of continuous inactivity (no motion detected
   by the MPU6050), the ride is saved to flash, WiFi connects, all pending
   rides are uploaded, and the device re-enters deep sleep.

4. **No GPS fix** — If a 3-D fix is not obtained within the timeout the
   device skips recording and goes straight back to sleep.

---

## Wiring diagram

```
ESP32-S3-DevKitC-1       MPU6050 breakout
─────────────────────    ─────────────────
3V3  ───────────────────► VCC
GND  ───────────────────► GND
GPIO 8  (SDA) ──────────► SDA          (4.7 kΩ pull-up to 3V3 recommended)
GPIO 9  (SCL) ──────────► SCL          (4.7 kΩ pull-up to 3V3 recommended)
GPIO 4  (INT) ◄──── 10 kΩ ── MPU6050 INT pin (active-high, push-pull)
                             AD0 ────► GND   (I²C address 0x68)


ESP32-S3-DevKitC-1       u-blox M10 GPS module
─────────────────────    ─────────────────────
3V3  ───────────────────► VCC  (or GPS module's own regulator input)
GND  ───────────────────► GND
GPIO 17 (UART1 TX) ─────► RX
GPIO 18 (UART1 RX) ◄──── TX
      (optional)
GPIO xx (power enable) ─► EN / RESET  (active-high, set GPS_POWER_GPIO)
```

> **GPIO 4 must be RTC-capable** — on the ESP32-S3-DevKitC-1 all GPIOs ≤ 21
> are RTC-capable and can be used as EXT0 wakeup sources.

> The MPU6050 INT pin is configured for active-high, latched, push-pull
> output.  A 10 kΩ pull-down to GND on the ESP32 side is recommended to keep
> the line firmly low when the device is disconnected.

---

## Pin summary

| Signal | DevKitC-1 GPIO | Direction | Notes |
|--------|---------------|-----------|-------|
| MPU6050 SDA | 8 | Bidirectional | I²C data |
| MPU6050 SCL | 9 | Output | I²C clock |
| MPU6050 INT | 4 | Input | RTC GPIO, EXT0 wakeup source |
| GPS UART TX | 17 | Output (to GPS RX) | 9600 baud default |
| GPS UART RX | 18 | Input (from GPS TX) | 9600 baud default |
| GPS power enable | – | Output | Optional; –1 = not used |

All assignments are configurable via `idf.py menuconfig` →
*ESP32 SSH LED Configuration → Bike Tracker Settings*.

---

## NVS storage

Ride data is stored in a dedicated **`bike_nvs`** NVS partition (5.9 MB),
completely separate from WiFi credentials.

| Partition | Offset | Size | Purpose |
|-----------|--------|------|---------|
| `nvs` | 0x009000 | 24 KB | WiFi credentials, system keys |
| `phy_init` | 0x00F000 | 4 KB | RF calibration |
| `factory` | 0x010000 | 2 MB | Firmware |
| `bike_nvs` | 0x210000 | **5.9 MB** | Ride data |

Track-points are stored as 12-byte records (lat, lon, speed × 10 in km/h).
At a 5-second log interval, a one-hour ride produces ~720 points (~9 KB).
**Estimated capacity: ~590 one-hour rides** before upload is required.

---

## Upload format

At the end of each ride the firmware HTTP-POSTs JSON to `TRACKER_UPLOAD_URL`:

```json
{
  "start_ts": 1712230800,
  "points": [
    { "lat": 514979820, "lon": -1234567, "speed_kmh": 245 },
    ...
  ]
}
```

`speed_kmh` is scaled × 10 (i.e. `245` = 24.5 km/h).
`lat` / `lon` are in degrees × 10⁻⁷ (WGS-84).

A ride is deleted from flash **only** on an HTTP 200 or 201 response.  If
WiFi is unavailable or the server is unreachable the ride is kept and retried
at the end of the next ride session.

---

## Configuration

All settings are under **ESP32 SSH LED Configuration → Bike Tracker Settings**
in `menuconfig` (visible only when the Bike Tracker variant is selected).

| Symbol | Default | Description |
|--------|---------|-------------|
| `MPU6050_I2C_PORT` | 0 | I²C peripheral number |
| `MPU6050_SDA_GPIO` | 8 | SDA pin |
| `MPU6050_SCL_GPIO` | 9 | SCL pin |
| `MPU6050_INT_GPIO` | 4 | INT pin — **must be RTC-capable** |
| `MPU6050_AD0_HIGH` | n | I²C address: n = 0x68, y = 0x69 |
| `GPS_UART_NUM` | 1 | UART peripheral number |
| `GPS_UART_RX_GPIO` | 18 | GPS RX pin |
| `GPS_UART_TX_GPIO` | 17 | GPS TX pin |
| `GPS_POWER_GPIO` | –1 | GPS power-enable pin (–1 = not used) |
| `GPS_FIX_TIMEOUT_S` | 60 | Seconds to wait for 3-D GPS fix |
| `TRACKER_LOG_INTERVAL_S` | 5 | Seconds between track-points |
| `TRACKER_INACTIVITY_TIMEOUT_S` | 30 | Seconds of no motion before stopping |
| `TRACKER_UPLOAD_URL` | "" | HTTP(S) endpoint for ride upload |

---

## Building this variant

In `menuconfig`, under **"ESP32 SSH LED Configuration"**, select:

> **Hardware variant → ESP32-S3 Bike Tracker (MPU6050 + u-blox GPS)**

Then build normally:

```powershell
idf.py --build-dir build_bike_tracker "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bike_tracker" build
idf.py --build-dir build_bike_tracker -p COM<N> flash
```

Or build all three variants at once:

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1
```

The output binary is `docs/firmware/esp32_bike_tracker.bin`.  Flash it
together with `docs/firmware/partition-table-bike_tracker.bin` (offset 0x8000)
and `docs/firmware/bootloader.bin` (offset 0x0).

---

## Flashing via web installer

Open <https://rvmey.github.io/esp32-s3-ssh-wled/>, select **Bike Tracker**,
connect the DevKitC-1 via USB and click **Install**.

---

## First-boot provisioning

1. Flash the firmware (web installer or `idf.py flash`).
2. Open the web installer page and click **Connect** (after flashing).
3. Enter your WiFi SSID and password when prompted.
4. Optionally enter your upload URL as the redirect/result URL.
5. The device will confirm its IP address, then go to deep sleep.

The upload URL can also be compiled in via `TRACKER_UPLOAD_URL` in
`menuconfig` if you prefer not to use Improv-WiFi for this.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| Boot loop (`rst:0x3` repeating) | MPU6050 not connected and INT pin floating high — connect MPU6050 or add a 10 kΩ pull-down from GPIO 4 to GND |
| No GPS fix | Poor sky view; move outdoors and allow up to 60 s for cold start |
| Ride not uploaded | Check `TRACKER_UPLOAD_URL` is set and server returns 200/201; ride stays in NVS and retries next session |
| MPU6050 WHO_AM_I mismatch | Wrong I²C address — set `MPU6050_AD0_HIGH=y` if AD0 is tied high |
| No wakeup on motion | INT pin pull-down too strong (< 1 kΩ); use 10 kΩ |
