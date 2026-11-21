// Service worker: bewaart de app in de telefoon zodat hij ook zonder internet opent.
// Verhoog VERSION bij elke nieuwe versie van de app, dan halen telefoons de nieuwe bestanden op.
const VERSION = 'rookmachines-v1.2';
const FILES = [
  './', 'index.html', 'manifest.webmanifest',
  'fonts/bebas-neue-400.woff2', 'fonts/oswald-400.woff2', 'fonts/oswald-500.woff2',
  'icons/icon-192.png', 'icons/icon-512.png', 'icons/icon-maskable-512.png', 'icons/apple-touch-icon.png'
];
self.addEventListener('install', e => {
  e.waitUntil(caches.open(VERSION).then(c => c.addAll(FILES)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(keys => Promise.all(keys.filter(k => k !== VERSION).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});
// Eerst het netwerk proberen (dan heb je altijd de nieuwste versie), anders de bewaarde kopie.
self.addEventListener('fetch', e => {
  if (e.request.method !== 'GET') return;
  // Firmware en installatiepagina niet in de telefoon bewaren (groot en alleen op de pc nodig)
  if (e.request.url.includes('/firmware/') || e.request.url.includes('installeren')) return;
  e.respondWith(
    fetch(e.request).then(r => {
      const copy = r.clone();
      caches.open(VERSION).then(c => c.put(e.request, copy));
      return r;
    }).catch(() => caches.match(e.request).then(r => r || caches.match('index.html')))
  );
});
