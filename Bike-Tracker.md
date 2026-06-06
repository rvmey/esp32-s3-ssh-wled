# ESP32-S3 Bike Tracker

This document is the canonical guide for the bike-tracker firmware variant.
The target hardware is ESP32-S3-DevKitC-1 (8 MB flash) with an MPU6050
motion sensor and a u-blox GPS module.

## Overview

The bike tracker sleeps by default, wakes on MPU6050 motion interrupt (EXT0)
or KY-003 hall sensor pulses (EXT1), records GPS track points during
activity, and uploads completed rides over WiFi.

High-level runtime flow:

1. First boot or reset:
   Initialize sensors, run Improv WiFi only if WiFi credentials are missing,
   then enter deep sleep.
2. Wake on motion/speed interrupt (`MPU6050_INT_GPIO` via EXT0 or
   `TRACKER_HALL_GPIO` via EXT1): Wait for GPS fix (up to
   `GPS_FIX_TIMEOUT_S`).
3. If a fix is acquired:
   Start a ride, log points every `TRACKER_LOG_INTERVAL_S`, and stop after
   `TRACKER_INACTIVITY_TIMEOUT_S` of inactivity.
4. On ride stop:
   Connect WiFi, upload pending rides, then return to deep sleep.

## Default Wiring

| Signal | Default GPIO | Notes |
|--------|--------------|-------|
| MPU6050 SDA | 8 | I2C data |
| MPU6050 SCL | 9 | I2C clock |
| MPU6050 INT | 4 | Must be RTC-capable for EXT0 wakeup |
| KY-003 DO | 2 | Must be RTC-capable for EXT1 wakeup |
| GPS UART TX | 17 | ESP32 TX to GPS RX |
| GPS UART RX | 18 | ESP32 RX from GPS TX |
| GPS power enable | -1 | Optional; -1 means unused |

All pins are configurable in menuconfig:
`ESP32 SSH LED Configuration -> Bike Tracker Settings`

## Ride Storage

Ride data is stored in a dedicated NVS partition named `bike_nvs`.

Partition table (`partitions_bike_tracker.csv`):

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data/nvs | 0x9000 | 0x6000 |
| phy_init | data/phy | 0xF000 | 0x1000 |
| factory | app/factory | 0x10000 | 0x200000 |
| bike_nvs | data/nvs | 0x210000 | 0x5F0000 |

Track points are stored as packed 12-byte records:

```c
typedef struct {
    int32_t  lat;        /* degrees x 1e-7 */
    int32_t  lon;        /* degrees x 1e-7 */
    int16_t  speed_kmh;  /* km/h x 10 */
    uint16_t _pad;
} track_point_t;
```

## Upload Behavior

- Upload destination is the compile-time setting `TRACKER_UPLOAD_URL`.
- If URL is empty, upload is skipped and rides remain stored.
- A ride is deleted only after HTTP status 200 or 201.
- Upload errors are non-fatal; failed rides remain in NVS for retry.
- Upload iterates ride indices from `0` to `ride_count - 1`.

Upload payload format:

```json
{
  "start_ts": 1712230800,
  "points": [
    { "lat": 514979820, "lon": -1234567, "speed_kmh": 245 }
  ]
}
```

## GPS and Motion Details

- `gps_ubx_wait_fix()` waits for a 3D fix (`fix_type >= 3`) before opening a
  ride.
- While logging, points are appended when GPS data is valid and
  `fix_type >= 2`.
- Inactivity is derived from MPU6050 motion interrupt status plus hall pulses
  sampled once per log interval.
- Speed uses hall pulses when available; GPS speed is used as fallback when no
  hall pulse is detected during the sample interval.

## Configuration (menuconfig)

All settings are under `ESP32 SSH LED Configuration -> Bike Tracker Settings`.

| Symbol | Default | Description |
|--------|---------|-------------|
| MPU6050_I2C_PORT | 0 | I2C peripheral number |
| MPU6050_SDA_GPIO | 8 | SDA pin |
| MPU6050_SCL_GPIO | 9 | SCL pin |
| MPU6050_INT_GPIO | 4 | RTC-capable GPIO for EXT0 wakeup |
| MPU6050_AD0_HIGH | n | I2C address select (0x68 low, 0x69 high) |
| GPS_UART_NUM | 1 | UART peripheral number |
| GPS_UART_RX_GPIO | 18 | GPS RX pin |
| GPS_UART_TX_GPIO | 17 | GPS TX pin |
| GPS_POWER_GPIO | -1 | Optional GPS power-enable pin |
| GPS_FIX_TIMEOUT_S | 60 | GPS fix timeout |
| TRACKER_LOG_INTERVAL_S | 5 | Point logging interval |
| TRACKER_INACTIVITY_TIMEOUT_S | 30 | Inactivity timeout |
| TRACKER_HALL_ENABLE | y | Enable KY-003 speed pulses |
| TRACKER_HALL_GPIO | 2 | KY-003 digital output pin (RTC-capable) |
| TRACKER_HALL_ACTIVE_HIGH | n | Set y if magnet detection drives output HIGH |
| TRACKER_WHEEL_CIRCUM_MM | 2105 | Wheel circumference used for speed conversion |
| TRACKER_HALL_PULSES_PER_REV | 1 | Pulses generated per wheel revolution |
| TRACKER_UPLOAD_URL | "" | Ride upload endpoint (empty disables upload) |

## Build and Flash

Build only the bike tracker variant:

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant bike_tracker
```

Artifact names used by the current installer manifest:

- `docs/firmware/bootloader-esp32s3.bin`
- `docs/firmware/partition-table-bike_tracker.bin`
- `docs/firmware/nvs_blank.bin`
- `docs/firmware/esp32_bike_tracker.bin`

If you are building directly with idf.py:

```powershell
idf.py --build-dir build_bike_tracker "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bike_tracker" build
idf.py --build-dir build_bike_tracker -p COM<N> flash
```

## Web Installer

Web installer page:
https://rvmey.github.io/esp32-s3-ssh-wled/

Bike tracker manifest:
`docs/manifest-bike_tracker.json`

## Key Source Files

- `main/bike_tracker.c` / `main/bike_tracker.h`
- `main/mpu6050.c` / `main/mpu6050.h`
- `main/gps_ubx.c` / `main/gps_ubx.h`
- `main/ride_log.c` / `main/ride_log.h`
- `main/ride_upload.c` / `main/ride_upload.h`
- `sdkconfig.bike_tracker`
- `partitions_bike_tracker.csv`
- `docs/manifest-bike_tracker.json`

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| Repeated wake/reset loop | MPU6050 INT line floating; verify wiring and pull-down |
| No wake on movement | Wrong INT GPIO or non-RTC-capable wake pin |
| No GPS fix | Poor sky view or antenna placement |
| Upload never happens | Empty `TRACKER_UPLOAD_URL` or WiFi connect failure |
| Rides never delete | Server not returning HTTP 200/201 |
