// Service Worker für ESP32 Heizungssteuerung
const CACHE_NAME = 'heizungssteuerung-v1';
const urlsToCache = [
  '/',
  '/index.html',
  '/manifest.json'
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => cache.addAll(urlsToCache))
  );
});

self.addEventListener('fetch', (event) => {
  // Network-first strategy for API calls
  if (event.request.url.includes('/api/')) {
    event.respondWith(
      fetch(event.request)
        .catch(() => caches.match(event.request))
    );
    return;
  }
  
  // Cache-first strategy for static assets
  event.respondWith(
    caches.match(event.request)
      .then((response) => response || fetch(event.request))
  );
});

self.addEventListener('push', (event) => {
  const data = event.data ? event.data.json() : {};
  const title = data.title || 'Heizungssteuerung';
  const options = {
    body: data.body || 'Neue Benachrichtigung',
    icon: data.icon || '/icon-192.png',
    badge: data.badge || '/icon-192.png',
    tag: 'heating-notification'
  };
  
  event.waitUntil(
    self.registration.showNotification(title, options)
  );
});

