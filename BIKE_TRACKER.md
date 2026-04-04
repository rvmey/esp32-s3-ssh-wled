# Bike Tracker Firmware

Third hardware variant for this repo: a GPS bike-ride logger that wakes from deep
sleep via MPU6050 motion interrupt, records track-points to NVS flash, and uploads
completed rides over WiFi.

---

## Hardware

| Component | Interface | Default GPIO |
|-----------|-----------|-------------|
| ESP32-S3-DevKitC-1 (8 MB flash) | — | — |
| MPU6050 accelerometer/gyro | I2C (port 0) | SDA 8 / SCL 9 |
| MPU6050 INT pin | GPIO input (RTC capable) | GPIO 4 |
| u-blox M10 GPS | UART1 | RX 18 / TX 17 |
| GPS power enable | GPIO output (optional) | –1 = not used |

All GPIO assignments are configurable via `idf.py menuconfig` →
*Bike Tracker Settings*.

---

## Operating Cycle

```
Power-on / reset
      │
      ▼
 Check wakeup cause
      │
      ├─ First boot / reset ──► Init MPU6050, configure wakeup ──► DEEP SLEEP
      │
      └─ EXT0 (MPU6050 INT) ──► ACQUIRING FIX
                                      │
                              GPS fix (≤ 60 s) ──► TRACKING ─────────────────┐
                                      │                  │                    │
                              timeout (no fix)       log point               inactivity
                                      │             every 5 s               timer (30 s)
                                      │                  │                    │
                                      └──────────────────▼────────────────────┘
                                                    STOPPING
                                                        │
                                              save ride to NVS
                                              connect WiFi
                                              upload pending rides
                                              disconnect WiFi
                                                        │
                                                   DEEP SLEEP
```

The device spends the vast majority of its time in deep sleep (~10 µA).
WiFi is activated only at the end of each ride.

---

## Ride Storage

### Partition layout (8 MB flash)

| Name | Type | Offset | Size | Purpose |
|------|------|--------|------|---------|
| `nvs` | data/nvs | 0x009000 | 24 KB | WiFi credentials, system keys |
| `phy_init` | data/phy | 0x00F000 | 4 KB | RF calibration |
| `factory` | app | 0x010000 | 2 MB | Firmware |
| `bike_nvs` | data/nvs | 0x210000 | **5.9 MB** | Ride data |

Ride data lives in a dedicated `bike_nvs` NVS partition, completely isolated
from system credentials.

### Capacity

| Parameter | Value |
|-----------|-------|
| Track-point size | 12 bytes (lat, lon, speed, padding) |
| Log interval | 5 seconds (configurable) |
| Points per 1-hour ride | ~720 |
| NVS overhead per ride | ~2 KB |
| **Estimated ride capacity** | **~590 one-hour rides** |

At 10 hours of riding per week that is over a year of storage before any
upload is required.

### Track-point format

```c
typedef struct {
    int32_t  lat;        /* degrees × 1e-7  (WGS-84) */
    int32_t  lon;        /* degrees × 1e-7  (WGS-84) */
    int16_t  speed_kmh;  /* speed in km/h × 10        */
    uint16_t _pad;
} track_point_t;         /* 12 bytes packed            */
```

### NVS keys (namespace `"bike_rides"`)

| Key | Type | Description |
|-----|------|-------------|
| `ride_count` | uint32 | Total rides ever started |
| `ride_NNNN` | blob | Array of `track_point_t` for ride N |
| `ride_NNNN_ts` | uint32 | Unix timestamp of ride start |

---

## Upload

Each completed ride is HTTP-POSTed as JSON to `TRACKER_UPLOAD_URL`:

```json
{
  "start_ts": 1712230800,
  "points": [
    { "lat": 514979820, "lon": -1234567, "speed_kmh": 245 },
    ...
  ]
}
```

- A ride is deleted from NVS only after a **HTTP 200 / 201** response.
- If the URL is empty, or WiFi fails, the ride remains in NVS and is retried
  next time the tracker stops.
- All pending rides (not just the latest) are uploaded in a single WiFi
  session.

---

## First-Boot Provisioning

On first boot (no WiFi credentials in NVS) the firmware starts
**Improv-WiFi Serial** before sleeping, matching the provisioning flow of the
other two variants. Open the [web installer](https://esp32-s3-ssh-screen.github.io)
and use the bike-tracker manifest to flash and provision.

The upload URL can be set as the redirect URL in the Improv-WiFi provisioning
result, or changed later via `idf.py menuconfig` and a reflash.

---

## Configuration (Kconfig)

All options are under **ESP32 SSH LED Configuration → Bike Tracker Settings**,
visible only when `HARDWARE_BIKE_TRACKER` is selected.

| Symbol | Default | Description |
|--------|---------|-------------|
| `MPU6050_I2C_PORT` | 0 | I2C peripheral number |
| `MPU6050_SDA_GPIO` | 8 | SDA pin |
| `MPU6050_SCL_GPIO` | 9 | SCL pin |
| `MPU6050_INT_GPIO` | 4 | INT pin — **must be RTC-capable** |
| `MPU6050_AD0_HIGH` | n | I2C address select: n = 0x68, y = 0x69 |
| `GPS_UART_NUM` | 1 | UART peripheral number |
| `GPS_UART_RX_GPIO` | 18 | GPS RX pin |
| `GPS_UART_TX_GPIO` | 17 | GPS TX pin |
| `GPS_POWER_GPIO` | –1 | GPS enable pin (–1 = not used) |
| `GPS_FIX_TIMEOUT_S` | 60 | Seconds to wait for 3-D fix |
| `TRACKER_LOG_INTERVAL_S` | 5 | Seconds between track-points |
| `TRACKER_INACTIVITY_TIMEOUT_S` | 30 | Seconds of no motion before stopping |
| `TRACKER_UPLOAD_URL` | "" | HTTP endpoint for ride upload |

---

## Source Files

### New files

| File | Purpose |
|------|---------|
| `main/bike_tracker.c/.h` | State machine (FSM), entry point |
| `main/mpu6050.c/.h` | MPU6050 I2C driver, wake-on-motion configuration |
| `main/gps_ubx.c/.h` | u-blox UBX binary protocol driver |
| `main/ride_log.c/.h` | NVS ride storage (write / read / delete) |
| `main/ride_upload.c/.h` | HTTP POST upload of stored rides |
| `sdkconfig.bike_tracker` | Variant sdkconfig fragment |
| `partitions_bike_tracker.csv` | Custom 8-MB partition table |
| `docs/manifest-bike_tracker.json` | Web installer manifest |

### Modified files

| File | Change |
|------|--------|
| `main/Kconfig.projbuild` | Add `HARDWARE_BIKE_TRACKER` choice + settings sub-menu |
| `main/CMakeLists.txt` | Add bike-tracker source list |
| `main/main.c` | `#elif CONFIG_HARDWARE_BIKE_TRACKER` branches in hw wrappers and `app_main` |
| `main/improv_wifi.c` | Version bump |
| `build.ps1` | 3rd variant entry; `Copy-Artifacts` copies per-variant partition table |

> **Note**: The MPU6050 also provides a gyroscope, but only the accelerometer
> channels are used for motion detection and wakeup. Gyro data is left powered
> down to minimise current draw during tracking.

---

## Build

```powershell
# Standard incremental build (all 3 variants)
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1

# Force full rebuild
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Clean
```

Output binaries written to `docs/firmware/`:

| File | Variant |
|------|---------|
| `esp32_ssh_devkitc.bin` | DevKitC-1 SSH server |
| `esp32_ssh_screen.bin` | JC3248W535 SSH server |
| `esp32_bike_tracker.bin` | **Bike tracker** |
| `partition-table-bike_tracker.bin` | Bike tracker partition table |

---

## Flashing

```powershell
# Flash bike-tracker (replace COMx with your port)
idf.py --build-dir build_bike_tracker -p COMx flash
```

Or use the web installer with `docs/manifest-bike_tracker.json`.

---

## Verification Checklist

- [ ] `idf.py --build-dir build_bike_tracker menuconfig` — `HARDWARE_BIKE_TRACKER` choice visible; all GPIO/timing options appear under *Bike Tracker Settings*
- [ ] `build.ps1 -Clean` — all 3 variants compile; `docs/firmware/esp32_bike_tracker.bin` and `partition-table-bike_tracker.bin` present
- [ ] Flash to DevKitC-1 — first boot: Improv-WiFi starts (no credentials stored)
- [ ] After provisioning — device goes to deep sleep (< 1 mA measured)
- [ ] Pull INT GPIO high (simulate MPU6050 motion) — EXT0 wakeup, GPS UART init visible in serial log
- [ ] Monitor serial during a simulated ride — track-points logged every 5 s
- [ ] After ride stops — WiFi connects, upload attempted, ride deleted from NVS on success
- [ ] Existing `devkitc` and `jc3248w535` builds unchanged

---

## Implementation Plan

### Step 1 — Kconfig: add `HARDWARE_BIKE_TRACKER` variant

Edit `main/Kconfig.projbuild`:

1. Add a third option to the `HARDWARE_VARIANT` choice:
   ```kconfig
   config HARDWARE_BIKE_TRACKER
       bool "ESP32-S3 Bike Tracker (MPU6050 + u-blox GPS)"
   ```
2. Add a new `menu "Bike Tracker Settings"` block, guarded by
   `depends on HARDWARE_BIKE_TRACKER`, containing all symbols from the
   [Configuration table](#configuration-kconfig) above.

---

### Step 2 — `sdkconfig.bike_tracker`

Create `sdkconfig.bike_tracker` at the repo root:

```kconfig
# Hardware variant: bike tracker (MPU6050 accelerometer + u-blox GPS)
CONFIG_HARDWARE_BIKE_TRACKER=y

# Console via USB Serial/JTAG (same as jc3248w535)
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y

# 8 MB flash (ESP32-S3-DevKitC-1)
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="8MB"

# Custom partition table with enlarged bike_nvs partition
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_bike_tracker.csv"

# Smaller main stack — no wolfSSH session task needed
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

---

### Step 3 — `partitions_bike_tracker.csv`

Create `partitions_bike_tracker.csv` at the repo root:

```csv
# Name,   Type, SubType,  Offset,   Size,    Flags
nvs,      data, nvs,      0x9000,   0x6000,
phy_init, data, phy,      0xF000,   0x1000,
factory,  app,  factory,  0x10000,  0x200000,
bike_nvs, data, nvs,      0x210000, 0x5F0000,
```

The `bike_nvs` partition (5.9 MB) is opened with
`nvs_flash_init_partition("bike_nvs")` in `ride_log.c`, keeping ride data
completely isolated from system credentials.

---

### Step 4 — `build.ps1`: add 3rd variant

In `build.ps1`, add a third object to the `$variants` array:

```powershell
[PSCustomObject]@{
    Name         = 'bike_tracker'
    Config       = 'sdkconfig.bike_tracker'
    BuildDir     = 'build_bike_tracker'
    OutputBin    = 'esp32_bike_tracker.bin'
    PartitionBin = 'partition-table-bike_tracker.bin'
}
```

In `Copy-Artifacts`, add logic to copy a per-variant partition table when the
`PartitionBin` property is present:

```powershell
if ($variant.PSObject.Properties['PartitionBin']) {
    $partSrc = Join-Path $buildDir 'partition_table\partition-table.bin'
    Copy-Item $partSrc (Join-Path $FirmwareDir $variant.PartitionBin) -Force
}
```

---

### Step 5 — `docs/manifest-bike_tracker.json`

Create the web installer manifest:

```json
{
  "name": "ESP32-S3 Bike Tracker",
  "version": "2.0.17",
  "new_install_improv_wifi": true,
  "new_install_improv_wait_time": 20,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "firmware/bootloader.bin",                    "offset": 0      },
        { "path": "firmware/partition-table-bike_tracker.bin",  "offset": 32768  },
        { "path": "firmware/nvs_blank.bin",                     "offset": 36864  },
        { "path": "firmware/esp32_bike_tracker.bin",            "offset": 65536  }
      ]
    }
  ]
}
```

---

### Step 6 — `main/mpu6050.h` / `main/mpu6050.c`

**`mpu6050.h`** — public API:
```c
esp_err_t mpu6050_init(void);
esp_err_t mpu6050_configure_wakeup(void);
void      mpu6050_clear_interrupt(void);
bool      mpu6050_is_active(void);
```

**`mpu6050.c`** — implementation notes:

| Operation | Register(s) | Value |
|-----------|-------------|-------|
| Verify identity | `WHO_AM_I` (0x75) | read → 0x68 or 0x69 |
| Wake from sleep | `PWR_MGMT_1` (0x6B) | write 0x00 (clear SLEEP) |
| Disable gyro | `PWR_MGMT_2` (0x6C) | write 0x07 (STBY_XG\|YG\|ZG) |
| Motion threshold | `MOT_THR` (0x1F) | write e.g. 0x08 (configurable) |
| Motion duration | `MOT_DUR` (0x20) | write 0x01 |
| INT pin config | `INT_PIN_CFG` (0x37) | write 0x20 (latch, active-high, push-pull) |
| Enable motion INT | `INT_ENABLE` (0x38) | write 0x40 |
| Clear interrupt | `INT_STATUS` (0x3A) | read (clears latch) |
| Low-power cycle | `PWR_MGMT_1` (0x6B) | write 0x20 (CYCLE=1, SLEEP=0) |
| Wakeup frequency | `PWR_MGMT_2` (0x6C) | bits [7:6] = 0b01 (5 Hz LP) |

I2C address: `0x68` when AD0 is low (default), `0x69` when
`CONFIG_MPU6050_AD0_HIGH=y`.

---

### Step 7 — `main/gps_ubx.h` / `main/gps_ubx.c`

**`gps_ubx.h`** — public API:
```c
typedef struct {
    int32_t  lat;        /* degrees × 1e-7 */
    int32_t  lon;        /* degrees × 1e-7 */
    int32_t  speed_mm_s; /* mm/s            */
    uint32_t unix_ts;    /* seconds UTC     */
    uint8_t  fix_type;   /* 0=none … 3=3D   */
    uint8_t  num_sv;     /* satellites used */
} gps_pvt_t;

esp_err_t gps_ubx_init(void);
esp_err_t gps_ubx_wait_fix(int timeout_s);
esp_err_t gps_ubx_get_pvt(gps_pvt_t *pvt);
void      gps_ubx_deinit(void);
```

**`gps_ubx.c`** — implementation notes:

- **Init**: install IDF UART driver on `CONFIG_GPS_UART_NUM` at 9600 baud;
  send UBX `CFG-PRT` to switch to 115200; reinstall driver at 115200; send
  `CFG-MSG` to enable `NAV-PVT` (class 0x01, id 0x07) at 1 Hz.
- **Frame format**: `0xB5 0x62 | class(1) | id(1) | len(2 LE) | payload | ck_a | ck_b`
  where `ck_a`/`ck_b` are an 8-bit Fletcher checksum over class through payload.
- **`wait_fix`**: continually reads and parses UBX frames; returns `ESP_OK`
  when a `NAV-PVT` with `fixType ≥ 3` is received; returns `ESP_ERR_TIMEOUT`
  after `timeout_s` seconds.
- **`get_pvt`**: returns the most-recently parsed `NAV-PVT` values without
  blocking.
- **`deinit`**: optionally sends `CFG-RST` (hot start, GPS stays powered) or
  toggles `CONFIG_GPS_POWER_GPIO` low; uninstalls UART driver.

---

### Step 8 — `main/ride_log.h` / `main/ride_log.c`

**`ride_log.h`** — public API:
```c
typedef struct {
    int32_t  lat;
    int32_t  lon;
    int16_t  speed_kmh;  /* km/h × 10 */
    uint16_t _pad;
} track_point_t;         /* 12 bytes   */

esp_err_t ride_log_init(void);          /* init bike_nvs partition    */
esp_err_t ride_log_start(uint32_t ts);  /* begin a new ride           */
esp_err_t ride_log_append(const track_point_t *pt);
uint32_t  ride_log_finish(void);        /* flush & return ride index  */
esp_err_t ride_log_count(uint32_t *n);
esp_err_t ride_log_read(uint32_t idx, track_point_t **pts,
                        size_t *n_pts, uint32_t *start_ts);
esp_err_t ride_log_delete(uint32_t idx);
```

**`ride_log.c`** — implementation notes:

- Opens the `bike_nvs` partition with `nvs_flash_init_partition("bike_nvs")`
  and all handles via `nvs_open_from_partition(...)`.
- Key scheme in namespace `"bike_rides"`:
  - `"ride_count"` — uint32, total rides ever started (monotonic).
  - `"rideXXXX"` — blob of `track_point_t[]` for ride index XXXX (zero-padded 4-digit decimal).
  - `"rideXXXX_t"` — uint32 Unix start timestamp.
- `ride_log_append` accumulates up to 512 track-points in a heap buffer and
  flushes to NVS blob on overflow or when `ride_log_finish()` is called.
- `ride_log_read` allocates a heap buffer (caller must free).
- `ride_log_delete` erases both `"rideXXXX"` and `"rideXXXX_t"` keys.

---

### Step 9 — `main/ride_upload.h` / `main/ride_upload.c`

**`ride_upload.h`** — public API:
```c
esp_err_t ride_upload_all(void);
```

**`ride_upload.c`** — implementation notes:

- Skips silently if `CONFIG_TRACKER_UPLOAD_URL` is empty.
- Iterates rides 0 … `ride_count - 1`; skips missing keys (already deleted).
- Builds JSON string in heap:
  `{"start_ts":<ts>,"points":[{"lat":<v>,"lon":<v>,"speed_kmh":<v>},…]}`
- Posts with `esp_http_client` (`HTTP_METHOD_POST`, `Content-Type: application/json`).
- Calls `ride_log_delete(idx)` only on HTTP 200 or 201 response.
- Uses `esp_tls` for HTTPS if the URL begins with `https://`; the server
  certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`) is assumed to
  already be enabled by the shared `sdkconfig.defaults`.

---

### Step 10 — `main/bike_tracker.h` / `main/bike_tracker.c`

**`bike_tracker.h`** — public API:
```c
void bike_tracker_run(void);   /* never returns (ends in deep sleep) */
```

**`bike_tracker.c`** — state machine:

```
bike_tracker_run()
│
├─ ride_log_init()
│
├─ wakeup == ESP_SLEEP_WAKEUP_EXT0 ?
│     Yes ──► STATE_ACQUIRING_FIX
│     No  ──► (first boot / reset)
│               wifi_has_stored_credentials() ?
│                 No  ──► improv_wifi_start()
│               mpu6050_init()
│               mpu6050_configure_wakeup()
│               ──► STATE_SLEEPING
│
STATE_ACQUIRING_FIX:
│   mpu6050_init() (clears any stale interrupt)
│   gps_ubx_init()
│   gps_ubx_wait_fix(CONFIG_GPS_FIX_TIMEOUT_S)
│     OK      ──► ride_log_start(pvt.unix_ts) → STATE_TRACKING
│     TIMEOUT ──► gps_ubx_deinit()            → STATE_SLEEPING
│
STATE_TRACKING:
│   loop every CONFIG_TRACKER_LOG_INTERVAL_S:
│     gps_ubx_get_pvt() → ride_log_append()
│     mpu6050_is_active() → reset or increment inactivity counter
│     inactivity counter ≥ CONFIG_TRACKER_INACTIVITY_TIMEOUT_S / interval
│       ──► STATE_STOPPING
│
STATE_STOPPING:
│   ride_log_finish()
│   gps_ubx_deinit()
│   wifi_connect()          (from wifi_manager.c)
│   ride_upload_all()
│   esp_wifi_stop()
│   ──► STATE_SLEEPING
│
STATE_SLEEPING:
│   mpu6050_clear_interrupt()
│   mpu6050_configure_wakeup()  (re-arm — needed if power-cycled)
│   esp_sleep_enable_ext0_wakeup(CONFIG_MPU6050_INT_GPIO, 1)
│   esp_deep_sleep_start()      ◄── never returns
```

---

### Step 11 — `main/CMakeLists.txt`: add bike-tracker sources

Add an `elseif` branch for the new variant:

```cmake
if(CONFIG_HARDWARE_JC3248W535)
    set(VARIANT_SRCS "screen_control.c")
elseif(CONFIG_HARDWARE_BIKE_TRACKER)
    set(VARIANT_SRCS
        "bike_tracker.c"
        "mpu6050.c"
        "gps_ubx.c"
        "ride_log.c"
        "ride_upload.c")
else()
    set(VARIANT_SRCS "led_control.c")
endif()
```

---

### Step 12 — `main/main.c`: wire in the bike-tracker entry point

1. Add include guard for the new variant:
   ```c
   #elif CONFIG_HARDWARE_BIKE_TRACKER
   #include "bike_tracker.h"
   ```

2. Add no-op stubs in `hw_init()`, `hw_set_color()`, and `hw_off()`:
   ```c
   #elif CONFIG_HARDWARE_BIKE_TRACKER
       /* handled inside bike_tracker_run() */
   ```

3. In `app_main()`, short-circuit after NVS init:
   ```c
   #if CONFIG_HARDWARE_BIKE_TRACKER
       bike_tracker_run();   /* never returns */
       return;
   #endif
   ```
   This skips WiFi provisioning, SSH server, and HTTP config for this variant.

---

### Step 13 — Version bump: 2.0.16 → 2.0.17

Four files must be updated atomically:

| File | Symbol |
|------|--------|
| `main/main.c` | `#define APP_VERSION "2.0.17"` |
| `main/improv_wifi.c` | `"2.0.17",  /* firmware version */` |
| `docs/manifest-devkitc.json` | `"version": "2.0.17"` |
| `docs/manifest-jc3248w535.json` | `"version": "2.0.17"` |

---

### Step 14 — Build and verify

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1
```

Expected output in `docs/firmware/`:

```
esp32_ssh_devkitc.bin
esp32_ssh_screen.bin
esp32_bike_tracker.bin
partition-table-bike_tracker.bin
bootloader.bin
partition-table.bin
```

Work through the [Verification Checklist](#verification-checklist) before committing.

---

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| Same repo, 3rd variant | Reuses `wifi_manager.c`, Improv-WiFi, build system, CI |
| MPU6050 (not BMA400) | Same I2C interface; ubiquitous module, widely available breakout boards; wake-on-motion interrupt available via `INT` pin |
| UBX binary protocol (not NMEA) | NAV-PVT delivers lat/lon/speed/time/fix-quality in a single atomic message; no string parsing required |
| Dedicated `bike_nvs` partition | Keeps ride data isolated from system NVS; 5.9 MB gives ~590 rides without any extra filesystem component |
| HTTP POST JSON | Simple server-side receiver; no broker or custom protocol needed |
| wolfSSH compiled but unused | Simplifies sdkconfig chain; avoids a separate `sdkconfig.defaults` for bike-tracker |
| Improv-WiFi provisioning | Consistent with other variants; allows upload URL to be set at first boot |
