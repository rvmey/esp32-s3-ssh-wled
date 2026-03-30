'use strict';

// Service worker: redirect any request for the legacy manifest.json to the
// current versioned manifest.  This fixes the case where GitHub Pages' Fastly
// CDN serves a stale index.html that still references manifest.json, even after
// the page has been updated to use manifest_v2.json.
//
// Once this SW is registered (on the first visit after a fresh index.html is
// served), it intercepts ALL subsequent manifest.json fetches for this origin
// and transparently returns manifest_v2.json content instead.

const CURRENT_MANIFEST = 'manifest_v2.json';

self.addEventListener('install', () => self.skipWaiting());

self.addEventListener('activate', event => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  if (url.pathname.endsWith('/manifest.json')) {
    const fixed = url.pathname.replace('/manifest.json', '/' + CURRENT_MANIFEST);
    event.respondWith(fetch(fixed));
  }
});
