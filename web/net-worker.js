// Network worker (see runtime/net.c NetRing and net.js): carries the game's packets for peers
// without a direct WebRTC link over its own relay WebSocket, so they never wait for the page's
// main thread (rendering, layout, garbage collection). It sleeps on the out ring with
// Atomics.waitAsync (or checks it every millisecond where that is missing) and writes incoming
// packets straight into the in ring, which wakes the game's network thread.
//
// Messages from the page: {init: {buffer, out, in, url, token}} once, then {peers: [[id, direct], ...]}
// whenever links change. To the page: {attached}, {closed}, {stats: {id: [tx, rx]}} every 2 s.
let H32, U8, OUT, IN, ws = null, open = false, url = '', token = '';
let peers = new Map();                 // id -> direct
const stats = new Map();               // id -> [tx, rx]
const BCAST = 'ffffffffffff';
const RING = (base) => ({ head: base >> 2, tail: (base >> 2) + 1, cap: 0, data: base + 16 });

function connect() {
  ws = new WebSocket(url); ws.binaryType = 'arraybuffer';
  ws.onopen = () => ws.send(JSON.stringify({ t: 'attach', token }));
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') { const m = JSON.parse(e.data); if (m.t === 'attached') { open = true; postMessage({ attached: true }); } else if (m.t === 'error') postMessage({ error: m.error }); return; }
    toGame(new Uint8Array(e.data));
  };
  ws.onclose = () => { open = false; postMessage({ closed: true }); setTimeout(connect, 1000); };
}
const count = (id, k) => { let s = stats.get(id); if (!s) stats.set(id, (s = [0, 0])); s[k]++; };

// relay -> game: [src 6][packet] into the in ring
function toGame(f) {
  if (f.length < 7) return;
  count(hexOf(f), 1);
  const n = f.length, h = Atomics.load(H32, IN.head), t = Atomics.load(H32, IN.tail), cap = IN.cap;
  if (cap - ((h - t) >>> 0) < n + 4) return;                          // full: drop (the game's redundancy copes)
  for (let i = 0; i < 4; i++) U8[IN.data + ((h + i) >>> 0) % cap] = (n >>> (8 * i)) & 255;
  for (let i = 0; i < n; i++) U8[IN.data + ((h + 4 + i) >>> 0) % cap] = f[i];
  Atomics.store(H32, IN.head, (h + 4 + n) | 0);
  Atomics.notify(H32, IN.head);
}
const hexOf = (f) => { let s = ''; for (let i = 0; i < 6; i++) s += f[i].toString(16).padStart(2, '0'); return s; };

// game -> relay: every record in the out ring, for peers without a direct link
function drain() {
  const cap = OUT.cap;
  let t = Atomics.load(H32, OUT.tail) >>> 0;
  const h = Atomics.load(H32, OUT.head) >>> 0;
  while (t !== h) {
    let n = 0;
    for (let i = 0; i < 4; i++) n |= U8[OUT.data + ((t + i) >>> 0) % cap] << (8 * i);
    const rec = new Uint8Array(n);
    for (let i = 0; i < n; i++) rec[i] = U8[OUT.data + ((t + 4 + i) >>> 0) % cap];
    t = (t + 4 + n) >>> 0;
    if (open) {
      const dst = hexOf(rec);
      if (dst === BCAST) {
        for (const [id, direct] of peers) if (!direct) { const f = rec.slice(); for (let i = 0; i < 6; i++) f[i] = parseInt(id.substr(2 * i, 2), 16); ws.send(f); count(id, 0); }
      } else if (peers.has(dst) && !peers.get(dst)) { ws.send(rec); count(dst, 0); }
    }
  }
  Atomics.store(H32, OUT.tail, t | 0);
}
async function loop() {
  const wait = Atomics.waitAsync;
  for (;;) {
    drain();
    const h = Atomics.load(H32, OUT.head);
    if (wait) { const r = wait(H32, OUT.head, h, 1000); if (r.async) await r.value; }
    else await new Promise((r) => setTimeout(r, 1));
  }
}

onmessage = (e) => {
  const d = e.data;
  if (d.init) {
    H32 = new Int32Array(d.init.buffer); U8 = new Uint8Array(d.init.buffer);
    OUT = RING(d.init.out); IN = RING(d.init.in);
    OUT.cap = H32[OUT.head + 2] >>> 0; IN.cap = H32[IN.head + 2] >>> 0;
    url = d.init.url; token = d.init.token;
    connect(); loop();
    setInterval(() => { if (stats.size) { postMessage({ stats: Object.fromEntries(stats) }); stats.clear(); } }, 2000);
  }
  if (d.peers) peers = new Map(d.peers);
};
