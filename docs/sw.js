'use strict';

// Service worker — keep this minimal and deterministic.
//
// Rewrite stale installer asset URLs to pinned versions.
// This protects users with an older cached index.html that references:
//   - manifest-core2.json → manifest-core2-2.0.278.json
//   - manifest-picture_frame.json → manifest-picture_frame-2.0.269.json
//   - any older Core2 firmware binary → esp32_core2_picture_frame-2.0.278.bin

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
    const target = 'https://raw.githubusercontent.com/rvmey/esp32-s3-ssh-wled/master/docs/manifest-core2-2.0.341.json';
    event.respondWith(fetch(target, { cache: 'no-store' }));
    return;
  }

  if (reqUrl.pathname.endsWith('/manifest-picture_frame.json')) {
    const target = 'https://raw.githubusercontent.com/rvmey/esp32-s3-ssh-wled/master/docs/manifest-picture_frame-2.0.317.json';
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
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.247.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.254.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.255.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.256.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.257.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.258.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.265.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.268.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.270.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.271.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.272.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.273.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.274.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.275.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.276.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.277.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.308.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.309.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.310.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.311.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.312.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.313.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.314.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.315.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.316.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.317.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.318.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.319.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.320.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.321.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.322.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.323.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.324.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.325.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.326.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.327.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.328.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.329.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.330.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.331.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.332.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.333.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.334.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_core2_picture_frame-2.0.335.bin')
  ) {
    const target = new URL('/esp32-s3-ssh-wled/firmware/esp32_core2_picture_frame-2.0.341.bin', self.location.origin);
    event.respondWith(fetch(target.toString(), { cache: 'no-store' }));
    return;
  }

  if (
    reqUrl.pathname.endsWith('/firmware/esp32_picture_frame.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_picture_frame-2.0.243.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_picture_frame-2.0.260.bin') ||
    reqUrl.pathname.endsWith('/firmware/esp32_picture_frame-2.0.265.bin')
  ) {
    const target = new URL('/esp32-s3-ssh-wled/firmware/esp32_picture_frame-2.0.317.bin', self.location.origin);
    event.respondWith(fetch(target.toString(), { cache: 'no-store' }));
  }
});
