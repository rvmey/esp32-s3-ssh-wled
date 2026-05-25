'use strict';

// Service worker — keep this minimal and deterministic.
//
// Rewrite stale Core2 installer asset URLs to pinned 2.0.215 files.
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
    const target = 'https://raw.githubusercontent.com/rvmey/esp32-s3-ssh-wled/master/docs/manifest-core2-2.0.215.json';
    event.respondWith(fetch(target, { cache: 'no-store' }));
    return;
  }

  if (
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.150.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.151.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.107.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.107-rebuild1.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.184.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.185.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.186.bin')
  ) {
    const target = new URL('/esp32-s3-ssh-wled/firmware/esp32_core2_picture_frame-2.0.215.bin', self.location.origin);
    event.respondWith(fetch(target.toString(), { cache: 'no-store' }));
  }
});
