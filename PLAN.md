# Plan: SSH Screen Color Control

## Goal
When the user runs `color red` (or any named/RGB/hex color) or `off` in the SSH shell,
fill the AXS15231 display with a solid color — in addition to the existing WS2812 LED behavior.

---

## Hardware Facts (from the YAML reference config)

| Signal   | GPIO |
|----------|------|
| SPI CLK  | 47   |
| SPI D0   | 21   |
| SPI D1   | 48   |
| SPI D2   | 40   |
| SPI D3   | 39   |
| SPI CS   | 45   |
| Backlight| 1    |
| Display  | AXS15231, 320 × 480, QSPI (quad SPI) |

> ⚠️ **GPIO 48 conflict:** `led_control.c` currently configures GPIO 48 as the WS2812
> data line (copied from the DevKitC-1 reference). On the JC3248W535 board, GPIO 48 is
> the display's D1 data line. Both cannot be used simultaneously. The LED pin definition
> in `led_control.c` will need to be updated to the correct GPIO for this board (or the
> LED feature disabled) before flashing.

---

## AXS15231 QSPI Protocol Summary

Every transaction on this chip begins with a 4-byte header sent in single-wire SPI:

| Byte | Value | Meaning |
|------|-------|---------|
| 0    | 0x02  | Command write (data in single-wire) OR |
|      | 0x32  | Command write (data in quad-wire) |
| 1    | 0x00  | Always 0 |
| 2    | CMD   | The register/command byte |
| 3    | 0x00  | Always 0 |

Following bytes are the payload (zero or more), in single or quad mode per byte 0.

Pixel writes use `cmd=0x2C` (RAMWR) with `0x32` prefix so pixel data streams in quad mode.

**Init sequence** (from ESPHome models.py, `AXS15231`):
```
0xBB  0x00 0x00 0x00 0x00 0x00 0x00 0x5A 0xA5   (unlock)
0xC1  0x33
0xBB  0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00   (lock)
```
Plus standard MIPI commands: `SLEEP_OUT (0x11)`, `COLMOD (0x3A) = 0x55` (RGB565),
`MADCTL (0x36)`, `DISPLAY_ON (0x29)`.

---

## Implementation Steps

### Step 1 — `screen_control.h` (new file)

Declare three public functions:

```c
esp_err_t screen_init(void);
void      screen_set_color(uint8_t r, uint8_t g, uint8_t b);
void      screen_off(void);   // fills screen black
```

### Step 2 — `screen_control.c` (new file)

1. **SPI bus init** — `spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO)`
   - `mosi` = GPIO 21 (D0), `data5`..`data7` pins for D1-D3 (quad), `sclk` = GPIO 47
   - IDF 5.x `spi_bus_config_t` has `data4_io_num`…`data7_io_num` for quad/octal.
   - Use `SPI_BUS_FLAG_QUAD` flag.

2. **Device add** — `spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi)`
   - `clock_speed_hz` = 40 MHz, `spics_io_num` = GPIO 45, `queue_size` = 7.

3. **Helper: `axs_write_cmd(cmd, data, len)`**
   - Builds the 4-byte header `[0x02, 0x00, cmd, 0x00]` as a single-wire transaction,
     followed by `len` data bytes (also single-wire for register writes).

4. **Helper: `axs_write_pixels(data, len_bytes)`**
   - Sends header `[0x32, 0x00, 0x2C, 0x00]` then `data` in quad mode
     (`SPI_TRANS_MODE_QIO` flag on the data transaction).

5. **Init sequence** inside `screen_init()`:
   - Backlight GPIO 1 — configure as output, drive high
   - Short reset delay
   - `SLEEP_OUT (0x11)`, delay 120 ms
   - AXS15231 unlock/init bytes above
   - `COLMOD (0x3A)` = 0x55 (16-bit RGB565)
   - `MADCTL (0x36)` = 0x00
   - `DISPLAY_ON (0x29)`, delay 20 ms
   - Fill screen black

6. **`screen_fill(r, g, b)`** — internal:
   - Compute `uint16_t px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)` (RGB565 big-endian)
   - Set column/row window: `CASET (0x2A)` [0,0,319], `RASET (0x2B)` [0,0,479]
   - Allocate a DMA line buffer (e.g. 320 × 2 bytes = 640 bytes)
   - Fill buffer with `px`, send 480 times via `axs_write_pixels`

7. **`screen_set_color` / `screen_off`** — thin wrappers over `screen_fill`.

### Step 3 — Wire into `ssh_server.c`

In `handle_command()`, two small additions:

- In the **`off`** branch: add `screen_off();` after `led_off();`
- In the **`color`** branch (success path): add `screen_set_color(r, g, b);` after `led_set_color(r, g, b);`

Add `#include "screen_control.h"` at the top of `ssh_server.c`.

### Step 4 — `main/CMakeLists.txt`

Add `"screen_control.c"` to the `SRCS` list.

### Step 5 — `main.c`

- Add `#include "screen_control.h"`
- Call `screen_init()` right after `led_init()`

### Step 6 — `sdkconfig.defaults` (optional tuning)

If pixel streaming stalls, add:
```
CONFIG_SPI_MASTER_IN_IRAM=y
```

---

## File Change Summary

| File | Change |
|------|--------|
| `main/screen_control.h` | **New** — public API |
| `main/screen_control.c` | **New** — AXS15231 QSPI driver + fill |
| `main/ssh_server.c` | Add include + 2 calls in `handle_command()` |
| `main/CMakeLists.txt` | Add `screen_control.c` to SRCS |
| `main/main.c` | Add include + `screen_init()` call |
| `led_control.c` | ⚠️ GPIO 48 → needs updating for JC3248W535 board |

---

## Open Questions Before Coding

1. **LED GPIO**: What GPIO is the RGB LED on the JC3248W535? (GPIO 48 is taken by the display.) Should LED control be removed or remapped?

LED control should be removed in one version, and included in the other version.  I want the option to produce a "ESP32-S3-DevKitC-1" version that implements the current function - to control the onboard RGB LED, or a "JC3248W535" version that controls the screen.  I may have other versions for other hardware in the future, so an easy method for users to choose from the flashing web page would be ideal.

2. **Backlight**: GPIO 1 is the backlight PWM. Should `screen_init` turn it fully on, or leave it for a separate `backlight` command?

screen_init should turn it fully on.

3. **SPI clock**: 40 MHz is what the ESPHome YAML uses. OK to keep?

Yes, 40 MHz is fine.