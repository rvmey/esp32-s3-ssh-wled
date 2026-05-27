'use strict';

// Service worker — keep this minimal and deterministic.
//
// Rewrite stale installer asset URLs to pinned versions.
// This protects users with an older cached index.html that references:
//   - manifest-core2.json → manifest-core2-2.0.248.json
//   - manifest-picture_frame.json → manifest-picture_frame-2.0.245.json
//   - any older Core2 firmware binary → esp32_core2_picture_frame-2.0.248.bin

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
    const target = 'https://raw.githubusercontent.com/rvmey/esp32-s3-ssh-wled/master/docs/manifest-core2-2.0.251.json';
    event.respondWith(fetch(target, { cache: 'no-store' }));
    return;
  }

  if (reqUrl.pathname.endsWith('/manifest-picture_frame.json')) {
    const target = 'https://raw.githubusercontent.com/rvmey/esp32-s3-ssh-wled/master/docs/manifest-picture_frame-2.0.245.json';
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
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.186.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.219.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.222.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.223.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.224.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.225.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.226.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.227.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.228.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.229.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.230.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.231.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.232.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.233.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.234.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.235.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.236.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.237.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.238.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.239.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.240.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.241.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.242.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.243.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.244.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.245.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.246.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.247.bin')
  ) {
    const target = new URL('/esp32-s3-ssh-wled/firmware/esp32_core2_picture_frame-2.0.251.bin', self.location.origin);
    event.respondWith(fetch(target.toString(), { cache: 'no-store' }));
    return;
  }

  if (
    reqUrl.pathname.endsWith('/firmware/esp32_picture_frame.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_picture_frame-2.0.243.bin')
  ) {
    const target = new URL('/esp32-s3-ssh-wled/firmware/esp32_picture_frame-2.0.245.bin', self.location.origin);
    event.respondWith(fetch(target.toString(), { cache: 'no-store' }));
  }
});
