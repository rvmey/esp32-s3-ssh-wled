# CLAUDE.md — Project Instructions

## Firmware version bump policy

Whenever **any firmware source file is modified** (files under `main/`, `CMakeLists.txt`, `sdkconfig.defaults`, `sdkconfig.devkitc`, `sdkconfig.jc3248w535`, patches under `patches/`, or `managed_components/`), you **must**:

### 1. Bump the patch version for any changed firmware variant

Increment `z` by 1 (e.g. `"2.0.1"` → `"2.0.2"`) in all four places:

- [main/main.c](main/main.c) — `#define APP_VERSION "x.y.z"`
- [main/improv_wifi.c](main/improv_wifi.c) — the version string literal passed to the Improv-WiFi service: `"x.y.z", /* firmware version */`
- [docs/manifest-devkitc.json](docs/manifest-devkitc.json) — `"version": "x.y.z"`
- [docs/manifest-jc3248w535.json](docs/manifest-jc3248w535.json) — `"version": "x.y.z"`

### 2. Build only the firmware variant currently being worked on

If the active variant is not clear from context, **ask before building**. Run with the ESP-IDF environment active:

```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant <name>
```

Variant names: `devkitc`, `jc3248w535`, `bike_tracker`, `picture_frame`

Example — DevKitC only:
```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant devkitc
```

Example — both SSH-screen variants:
```powershell
. C:\esp\v6.0\esp-idf\export.ps1 2>&1 | Out-Null ; .\build.ps1 -Variant devkitc, jc3248w535
```

Output binaries are written to [docs/firmware/](docs/firmware/):
- `esp32_ssh_devkitc.bin` — DevKitC-1 variant
- `esp32_ssh_screen.bin` — JC3248W535 variant
- `esp32_bike_tracker.bin` — Bike Tracker variant
- `esp32_picture_frame.bin` — Picture Frame variant
- `bootloader.bin` and `partition-table.bin` — shared, taken from the last built variant

### 3. Stage, commit, and push

Include only the binaries for the variant(s) that were built. Include the version in the commit message.

DevKitC only:
```powershell
git add main/main.c main/improv_wifi.c docs/manifest-devkitc.json docs/firmware/esp32_ssh_devkitc.bin docs/firmware/bootloader.bin docs/firmware/partition-table.bin
git commit -m "Bump DevKitC firmware version to x.y.z"
git push
```

JC3248W535 only:
```powershell
git add main/main.c main/improv_wifi.c docs/manifest-jc3248w535.json docs/firmware/esp32_ssh_screen.bin docs/firmware/bootloader.bin docs/firmware/partition-table.bin
git commit -m "Bump JC3248W535 firmware version to x.y.z"
git push
```

Only include `docs/manifest-*.json` files for the variant(s) whose version was bumped.

### 4. Update flasher-page pinning to avoid cache issues

For any variant that uses a stable installer alias (e.g. `manifest-core2.json`), update all pinning locations so browsers and stale service workers cannot serve an old release:

- `docs/manifest-<variant>.json`:
  - Set `"version"` to the new `x.y.z`
  - Point firmware `parts[].path` to the new versioned binary (e.g. `firmware/esp32_core2_picture_frame-x.y.z.bin`)
- Create/update the pinned manifest `docs/manifest-<variant>-x.y.z.json` with the same version and binary path
- [docs/index.html](docs/index.html):
  - Update `fixedManifestMap` to point `manifest-<variant>.json` → `manifest-<variant>-x.y.z.json`
  - Update `fixedVersionMap` for `manifest-<variant>.json` to `x.y.z`
  - Keep manifest fetches using `{ cache: 'no-store' }`
- [docs/sw.js](docs/sw.js):
  - Update rewrite targets/comments to the new pinned manifest and versioned firmware binary
  - Keep rewritten fetches using `{ cache: 'no-store' }`

This step is required whenever installer-facing firmware for that variant changes.

## Project structure

- [main/main.c](main/main.c) — defines `APP_VERSION` (single source of truth for the version string)
- [build.ps1](build.ps1) — builds one or more variants via `-Variant <name>`; pass `-Clean` to force a full rebuild
- [docs/firmware/](docs/firmware/) — pre-built binaries committed to the repo for easy flashing
- Hardware variants:
  - **DevKitC-1** — no display, uses `sdkconfig.devkitc`, outputs `esp32_ssh_devkitc.bin`
  - **JC3248W535** — 3.5" touch display, uses `sdkconfig.jc3248w535`, outputs `esp32_ssh_screen.bin`
- [patches/](patches/) + [apply-patches.ps1](apply-patches.ps1) — patches applied over managed components; re-apply after `idf.py update-dependencies`
