# Fix: "Connect to Wi-Fi" Option Missing After Flash

## Symptom

After flashing the firmware through the web installer, clicking **Next** shows only:

- Install ESP32-S3 SSH Server
- Logs & Console

The **Connect to Wi-Fi** option never appears.

---

## Root Cause

The **Connect to Wi-Fi** option is provided by the [Improv Wi-Fi](https://www.improv-wifi.com/) protocol.  It appears only when the firmware is actively advertising Improv over the serial port — which only happens when **no Wi-Fi credentials are stored in NVS**.

The installer page (`index.html`) references a manifest file that tells `esp-web-tools` which firmware parts to flash and in what order.  The manifest must include `nvs_blank.bin` at offset `0x9000` to erase any previously stored Wi-Fi credentials before the firmware runs.

### Why the CDN breaks this

The installer is hosted on **GitHub Pages**, which uses **Fastly** as its CDN.  Fastly caches files aggressively.  When `manifest.json` or `index.html` is updated and pushed to GitHub, the CDN does not always purge its cached copy in a timely manner (observed to persist for many hours or days in practice, despite GitHub Pages advertising a 10-minute `max-age`).

Because `index.html` was cached from an early version, users received:

```
index.html  (stale CDN copy) → manifest="manifest.json"
manifest.json (stale CDN copy, v1.0.0) → no nvs_blank.bin, has new_install_prompt_erase: true
```

Result:

1. `nvs_blank.bin` never flashed → old Wi-Fi credentials survive.
2. `new_install_prompt_erase: true` showed an erase checkbox — if unchecked, `installErase = false` → `esp-web-tools` routed post-flash to the dashboard, not the Wi-Fi provisioning form.
3. Device booted with existing credentials → connected to Wi-Fi → started SSH server → Improv not running.
4. Browser saw no Improv → showed dashboard with no **Connect to Wi-Fi** option.

---

## Fix History

### Attempt 1 — Rename the manifest (commit `95a720a`)

A new file `manifest_v2.json` was created (identical content to the corrected `manifest.json`) and `index.html` was updated to reference it.

**Why this worked initially**: Fastly had no cached entry for the new URL `manifest_v2.json`, so it fetched from origin and returned correct content.

**Why it was not enough**: `index.html` itself was also stale in the CDN.  Users continued to receive the old `index.html` which still referenced `manifest.json` (v1.0.0).  The rename only fixed users who received a fresh copy of `index.html`.

### Attempt 2 — CDN-proof dynamic manifest loading (current, commit after v1.7)

Three separate mechanisms were added so the correct manifest is served **regardless of how stale `index.html` is in the CDN**:

#### 1. `config.json` + timestamp cache-buster in `index.html`

`config.json` is a tiny file that holds only the current manifest filename:

```json
{ "manifest": "manifest_v2.json" }
```

`index.html` now fetches this file at page load with `Date.now()` appended:

```js
fetch('config.json?t=' + Date.now())
  .then(r => r.json())
  .then(cfg => btn.setAttribute('manifest', cfg.manifest))
  .catch(() => btn.setAttribute('manifest', 'manifest_v2.json'));
```

Because the URL is unique on every request, Fastly cannot serve a cached copy.  It is forced to fetch from origin, which always returns the current `config.json`.  Future manifest updates only require:

1. Deploying a new `manifest_vN.json`.
2. Updating `config.json` to point to it.

`index.html` never needs to be touched again.

#### 2. Service worker (`sw.js`)

A service worker is registered on the first page load that includes the new `index.html`:

```js
// sw.js – intercepts fetch for manifest.json and returns manifest_v2.json
self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  if (url.pathname.endsWith('/manifest.json')) {
    event.respondWith(fetch(url.pathname.replace('/manifest.json', '/manifest_v2.json')));
  }
});
```

Once registered, it persists across browser sessions.  Even if a user somehow triggers the old `manifest.json` URL (e.g., via a very-long-lived cache), the service worker intercepts the request and transparently serves `manifest_v2.json` instead.

`skipWaiting()` + `clients.claim()` ensure the SW activates immediately without waiting for a page reload.

#### 3. Cache-Control meta tags in `index.html`

```html
<meta http-equiv="Cache-Control" content="no-cache, max-age=0" />
<meta http-equiv="Pragma" content="no-cache" />
```

These do not control Fastly (which ignores `<meta>` tags), but they prevent the browser itself from caching `index.html` at the HTTP level on subsequent visits.

---

## How to Deploy a Future Firmware Update

1. Build and copy the new binary to `docs/firmware/esp32_ssh_led_vX.Y.bin`.
2. Copy `docs/manifest_v2.json` to `docs/manifest_vN.json` (where N = next version number).
3. Update `docs/manifest_vN.json`: bump `"version"` and change the binary filename.
4. Update `docs/config.json`: set `"manifest": "manifest_vN.json"`.
5. Commit and push.

`index.html` and `sw.js` do not need to change.

---

## Workaround for Users Experiencing the Issue

If a user is stuck on the stale installer page and the CDN has not yet propagated:

1. Hard-refresh the page: **Ctrl + Shift + R** (Windows/Linux) or **Cmd + Shift + R** (macOS).
2. Or append a dummy query parameter to the URL: `https://rvmey.github.io/esp32-s3-ssh-wled/?v=1`

Either approach causes the browser to bypass its local cache and request `index.html` directly from Fastly's origin (GitHub Pages), which always has the latest content.
