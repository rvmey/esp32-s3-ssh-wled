'use strict';

// Service worker — currently a no-op.  Previously redirected manifest.json
// to manifest_v2.json for CDN cache-busting.  Kept registered so the browser
// replaces any previously-installed version that was doing the redirect.

self.addEventListener('install', () => self.skipWaiting());

self.addEventListener('activate', event => {
  event.waitUntil(self.clients.claim());
});
