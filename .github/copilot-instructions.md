# GitHub Copilot Instructions

## Firmware version bump policy

Whenever **any firmware source file is modified** (files under `main/`, `CMakeLists.txt`, `sdkconfig.defaults`, `sdkconfig.devkitc`, `sdkconfig.jc3248w535`, patches under `patches/`, or `managed_components/`), you **must**:

1. **Bump the patch version for any changed firmware variant** in all four places — increment `z` by 1 (e.g. `"2.0.1"` → `"2.0.2"`):

   - `main/main.c`:
     ```c
     #define APP_VERSION "x.y.z"
     ```
   - `main/improv_wifi.c` (the firmware version string literal passed to the Improv-WiFi service):
     ```c
     "x.y.z",               /* firmware version */
     ```
   - `docs/manifest-devkitc.json`:
     ```json
     "version": "x.y.z",
     ```
   - `docs/manifest-jc3248w535.json`:
     ```json
     "version": "x.y.z",
     ```

2. **Build only firmware variants that have changed** by running (with the ESP-IDF environment active):
   ```powershell
   . C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1
   ```
   This builds `build_devkitc/` and `build_jc3248w535/` and copies the output binaries to `docs/firmware/`:
   - `docs/firmware/esp32_ssh_devkitc.bin`
   - `docs/firmware/esp32_ssh_screen.bin`
   - `docs/firmware/bootloader.bin`
   - `docs/firmware/partition-table.bin`

3. **Stage and commit everything**, including the source changes, version bump, and updated binaries:
   ```powershell
   git add main/main.c main/improv_wifi.c docs/manifest-devkitc.json docs/manifest-jc3248w535.json docs/firmware/esp32_ssh_devkitc.bin docs/firmware/esp32_ssh_screen.bin docs/firmware/bootloader.bin docs/firmware/partition-table.bin
   git commit -m "<short description of change>"
   git push
   ```

## Project structure notes

- `main/main.c` — defines `APP_VERSION` (single source of truth for the version string)
- `build.ps1` — builds both hardware variants; pass `-Clean` to force a full rebuild
- `docs/firmware/` — pre-built binaries committed to the repo for easy flashing
- Two hardware variants:
  - **DevKitC-1** — no display, uses `sdkconfig.devkitc`, outputs `esp32_ssh_devkitc.bin`
  - **JC3248W535** — 3.5" touch display, uses `sdkconfig.jc3248w535`, outputs `esp32_ssh_screen.bin`
- `patches/` + `apply-patches.ps1` — patches applied over managed components; re-apply after `idf.py update-dependencies`
