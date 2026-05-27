# Core2 BT A2DP Crash — Root Cause and Fix

**Symptom:** M5Stack Core2 reboots with "Interrupt WDT timeout on CPU0" a few seconds after
BT A2DP audio streaming begins while WiFi is also connected.

**Fixed in:** firmware 2.0.242  
**Affected versions:** any build with `touch_poll_task` using a 10 ms I2C timeout while
BT A2DP is active

---

## How to decode a crash like this

The critical register is **EPC1** in the Core 0 dump:

```
EPC1 : 0x40147bbb
```

EPC1 captures the program counter of the level-1 interrupt handler that was running when
the level-5 INT WDT fired. Run `addr2line` on the firmware ELF:

```
xtensa-esp32-elf-addr2line -e build_core2/esp32_ssh_led.elf -f 0x40147bbb
```

Output:
```
rtcio_ll_pulldown_disable
esp_hal_gpio/esp32/include/hal/rtc_io_ll.h:235
```

Decode the full backtrace the same way to get the call chain that was executing in the
normal (non-ISR) task at the moment the watchdog fired.

---

## What was happening

### The normal task (backtrace)

| Address | Function | File |
|---------|----------|------|
| 0x400e9f6f | `apply_scroll_abs` | `screen_control_core2.c:385` |
| 0x40145653 | `i2c_master_write_read_device` | `i2c.c:1153` |
| 0x401454f4 | `i2c_master_cmd_begin` | `i2c.c:1642` |
| 0x40144e71 | `i2c_master_clear_bus` | `i2c.c:700` |
| 0x40144252 | `i2c_hw_disable` | `i2c.c:272` |

`touch_poll_task` runs every 20 ms, reading the FT6336U touch controller via
`i2c_master_write_read_device` with a **10 ms timeout**.

### The ISR (EPC1)

`rtcio_ll_pulldown_disable` inside `i2c_hw_disable` — the I2C ISR was also trying to
disable the I2C hardware peripheral.

### The deadlock

1. Touch task calls `i2c_master_write_read_device(... pdMS_TO_TICKS(10))`.
2. Under BT A2DP + WiFi coexistence load, the I2C ISR is delayed past 10 ms by RF
   arbitration (BT slot interval ≈ 7.5 ms).
3. Timeout fires. `i2c_master_cmd_begin` detects the previous transfer timed out and calls
   `i2c_master_clear_bus → i2c_hw_disable`.
4. `i2c_hw_disable` acquires the driver spinlock (`I2C_ENTER_CRITICAL`).
5. At this exact moment the I2C hardware fires its interrupt; the ISR fires and also calls
   `i2c_hw_disable`, which tries to acquire the **same spinlock**.
6. The ISR spins forever (the task that holds the spinlock cannot run because the ISR is
   preempting it).
7. After 2000 ms the INT WDT fires.

---

## False leads investigated

| Attempted fix | Result |
|---------------|--------|
| CPU at 240 MHz (was 160 MHz) | No effect — crash unchanged |
| `esp_wifi_set_ps(WIFI_PS_NONE)` at stream start | Delayed crash from ~3 s to ~9 s, still crashed |
| Larger BT PCM ring buffer | No effect |
| `esp_coex_preference_set(ESP_COEX_PREFER_BT)` | No effect |
| Raised INT WDT timeout to 2000 ms | Just delayed crash |

All of these targeted the BT/WiFi coexistence layer, which was a red herring. The crash
address `0x40147bbb` is in the application's flash-mapped code segment (ESP-IDF I2C
driver), **not** in the ESP32 Bluetooth ROM as initially assumed. `addr2line` was
required to confirm this.

---

## The fix

In `main/screen_control_core2.c`, `touch_read_point()`:

```c
// Before (10 ms — too tight under BT load):
i2c_master_write_read_device(..., pdMS_TO_TICKS(10))

// After (50 ms — gives the I2C ISR 6+ BT slot intervals to complete):
i2c_master_write_read_device(..., pdMS_TO_TICKS(50))
```

At 400 kHz I2C, a 1-byte read completes in ~40 µs. Even if the I2C ISR is blocked for one
full BT RF slot (7.5 ms), 50 ms provides ample margin. The ISR completes normally and
`i2c_master_clear_bus` is never reached.

---

## Key diagnostic commands

```powershell
# Decode any address from a crash dump
. C:\esp\v6.0\esp-idf\export.ps1
xtensa-esp32-elf-addr2line -e build_core2\esp32_ssh_led.elf -f <address> [<address> ...]

# Add -f to also print function names
xtensa-esp32-elf-addr2line -e build_core2\esp32_ssh_led.elf -f 0x40147bbb
```

Always decode **EPC1** first — it shows where the stuck ISR was executing when the
watchdog fired, which is the actual site of the hang. The backtrace frames show the
interrupted task's call stack, useful for understanding what triggered the ISR.
