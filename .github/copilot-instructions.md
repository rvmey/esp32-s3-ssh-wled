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

2. **Build only the firmware variant currently being worked on.** If the active variant is not clear from context, ask before building. Run (with the ESP-IDF environment active):
   ```powershell
   . C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant <name>
   ```
   Valid variant names: `devkitc`, `jc3248w535`, `bike_tracker`, `picture_frame`.

   Example — working on the DevKitC variant:
   ```powershell
   . C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant devkitc
   ```
   Example — working on both SSH-screen variants at once:
   ```powershell
   . C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant devkitc, jc3248w535
   ```

   Output binaries are written to `docs/firmware/`:
   - `docs/firmware/esp32_ssh_devkitc.bin` — DevKitC-1 variant
   - `docs/firmware/esp32_ssh_screen.bin`  — JC3248W535 variant
   - `docs/firmware/esp32_bike_tracker.bin` — Bike Tracker variant
   - `docs/firmware/esp32_picture_frame.bin` — Picture Frame variant
   - `docs/firmware/bootloader.bin` and `docs/firmware/partition-table.bin` — shared, taken from the last built variant

3. **Stage and commit everything**, including only the binaries for the variant(s) that were built. For example, if only the DevKitC variant was built:
   ```powershell
   git add main/main.c main/improv_wifi.c docs/manifest-devkitc.json docs/firmware/esp32_ssh_devkitc.bin docs/firmware/bootloader.bin docs/firmware/partition-table.bin
   git commit -m "<short description of change>"
   git push
   ```
   If only the JC3248W535 variant was built:
   ```powershell
   git add main/main.c main/improv_wifi.c docs/manifest-jc3248w535.json docs/firmware/esp32_ssh_screen.bin docs/firmware/bootloader.bin docs/firmware/partition-table.bin
   git commit -m "<short description of change>"
   git push
   ```
   Only include `docs/manifest-*.json` files for the variant(s) whose version was bumped.

## Project structure notes

- `main/main.c` — defines `APP_VERSION` (single source of truth for the version string)
- `build.ps1` — builds one or more variants via `-Variant <name>`; pass `-Clean` to force a full rebuild
- `docs/firmware/` — pre-built binaries committed to the repo for easy flashing
- Two hardware variants:
  - **DevKitC-1** — no display, uses `sdkconfig.devkitc`, outputs `esp32_ssh_devkitc.bin`
  - **JC3248W535** — 3.5" touch display, uses `sdkconfig.jc3248w535`, outputs `esp32_ssh_screen.bin`
- `patches/` + `apply-patches.ps1` — patches applied over managed components; re-apply after `idf.py update-dependencies`
