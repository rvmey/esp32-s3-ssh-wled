# TRIGGERcmd CYD Display

This variant targets the ESP32-2432S028R (Cheap Yellow Display) and uses a
Core2-compatible TRIGGERcmd display profile.

It is intended to stay as close as possible to the Core2 firmware behavior:

- Same TRIGGERcmd Socket.IO display workflow
- Same command set (`text`, `color`, `textcolor`, `fontsize`, `landscape`,
  `portrait`, `jpeg`, `save`)
- Same ESP32 target family and partitioning strategy

## Build

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant cyd
```

Output firmware path:

- `docs/firmware/esp32_cyd_picture_frame.bin`

## Installer manifest

- `docs/manifest-cyd.json`

## Notes

- This is a new board profile that currently follows the Core2 runtime feature
  set for parity.
- CYD board-to-board wiring can vary by vendor. Use menuconfig under
  ESP32 SSH LED Configuration -> CYD Settings to adjust LCD SPI pins,
  optional LCD reset pin, and backlight GPIO polarity.
