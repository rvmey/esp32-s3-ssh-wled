'use strict';

// Service worker — keep this minimal and deterministic.
//
// Rewrite stale Core2 installer asset URLs to pinned 2.0.183 files.
// This protects users with an older cached index.html that references:
//   - manifest-core2.json
//   - firmware/esp32_core2_picture_frame.bin
//   - firmware/esp32_core2_picture_frame-2.0.150.bin
//   - firmware/esp32_core2_picture_frame-2.0.151.bin
//   - firmware/esp32_core2_picture_frame-2.0.152.bin

self.addEventListener('install', () => self.skipWaiting());

self.addEventListener('activate', event => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', event => {
  const reqUrl = new URL(event.request.url);

  if (reqUrl.origin !== self.location.origin) {
    return;
  }

  if (reqUrl.pathname.endsWith('/manifest-core2.json')) {
    const target = 'https://raw.githubusercontent.com/rvmey/esp32-s3-ssh-wled/master/docs/manifest-core2-2.0.183.json';
    event.respondWith(fetch(target, { cache: 'no-store' }));
    return;
  }

  if (
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.150.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.151.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.107.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.107-rebuild1.bin')
  ) {
    const target = new URL('/esp32-s3-ssh-wled/firmware/esp32_core2_picture_frame-2.0.183.bin', self.location.origin);
    event.respondWith(fetch(target.toString(), { cache: 'no-store' }));
  }
});
*** Add File: c:\appdev\esp\esp32-s3-ssh-wled\docs\manifest-core2-2.0.183.json
{
  "name": "TRIGGERcmd Core2 Display (M5Stack Core2 for AWS)",
  "version": "2.0.183",
  "new_install_improv_wifi": false,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "firmware/bootloader-esp32.bin",             "offset": 4096  },
        { "path": "firmware/partition-table-esp32.bin",        "offset": 32768 },
        { "path": "firmware/nvs_blank.bin",                    "offset": 36864 },
        { "path": "firmware/esp32_core2_picture_frame-2.0.183.bin", "offset": 65536 }
      ]
    }
  ]
}
