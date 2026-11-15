// Network worker (see runtime/net.c NetRing and net.js): carries the game's packets off the
// page's main thread, so they never wait for it (rendering, layout, garbage collection):
//  - the direct links: the page hands it each WebRTC data channel as soon as it is made
//    (a transferable RTCDataChannel; browsers without that keep the channel on the page);
//  - the relay: its own WebSocket to the server, attached to the page's place in the room.
// Once the game runs it sleeps on the out ring with Atomics.waitAsync (or checks it every
// millisecond where that is missing) and writes incoming packets straight into the in ring,
// which wakes the game's network thread. Before that, packets are not expected.
//
// From the page: {dc, id} (a data channel, transferred), {init: {buffer, out, in, url, token}}
// once the game runs, {peers: [[id, onPage], ...]} whenever links change (onPage: the page
// sends to that peer's direct link itself), {token} after the page resumed its place.
// To the page: {dcOpen: id}, {dcClosed: id}, {attached}, {closed}, {error},
// {stats: {id: [tx, txRelay, rx, rxRelay, gaps, recovered]}} every 2 s (gaps: packets missing
// from the direct link, recovered: of those, carried by a later message).
import { frame, unframe } from './net.js';

let H32, U8, OUT, IN, ws = null, open = false, url = '', token = '';
let peers = new Map();                 // id -> the page sends to this one's direct link
const dcs = new Map();                 // id -> { dc, tx, rx }
const stats = new Map();               // id -> [tx, txRelay, rx, rxRelay, gaps, recovered]
const BCAST = 'ffffffffffff';
const RING = (base) => ({ head: base >> 2, tail: (base >> 2) + 1, cap: 0, data: base + 16 });
const count = (id, k, n = 1) => { let s = stats.get(id); if (!s) stats.set(id, (s = [0, 0, 0, 0, 0, 0])); s[k] += n; };
const hexOf = (f) => { let s = ''; for (let i = 0; i < 6; i++) s += f[i].toString(16).padStart(2, '0'); return s; };
const unhex = (s) => Uint8Array.from(s.match(/../g), (h) => parseInt(h, 16));

function connect() {
  ws = new WebSocket(url); ws.binaryType = 'arraybuffer';
  ws.onopen = () => ws.send(JSON.stringify({ t: 'attach', token }));
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') { const m = JSON.parse(e.data); if (m.t === 'attached') { open = true; postMessage({ attached: true }); } else if (m.t === 'error') postMessage({ error: m.error }); return; }
    const f = new Uint8Array(e.data);
    if (f.length > 6) { count(hexOf(f), 3); toGame(f.subarray(0, 6), f.subarray(6)); }
  };
  ws.onclose = () => { open = false; postMessage({ closed: true }); setTimeout(connect, 1000); };
}

// a data channel of ours: its packets go straight to the game
function addDc(id, dc) {
  const q = { dc, tx: { seq: 0, hist: [] }, rx: { hi: null, seen: new Set(), gaps: 0, recovered: 0 } };
  dcs.set(id, q);
  dc.binaryType = 'arraybuffer';
  const src = unhex(id);
  dc.onopen = () => postMessage({ dcOpen: id });
  dc.onclose = () => { if (dcs.get(id) === q) dcs.delete(id); postMessage({ dcClosed: id }); };
  dc.onerror = () => {};
  dc.onmessage = (ev) => {
    const g = q.rx.gaps, r = q.rx.recovered;
    for (const pkt of unframe(q.rx, new Uint8Array(ev.data))) { count(id, 2); toGame(src, pkt); }
    if (q.rx.gaps !== g) count(id, 4, q.rx.gaps - g);
    if (q.rx.recovered !== r) count(id, 5, q.rx.recovered - r);
  };
  if (dc.readyState === 'open') postMessage({ dcOpen: id });
}

// -> game: [src 6][packet] into the in ring
function toGame(src, pkt) {
  if (!IN) return;                                                   // the game is not running yet
  const n = 6 + pkt.length, h = Atomics.load(H32, IN.head), t = Atomics.load(H32, IN.tail), cap = IN.cap;
  if (cap - ((h - t) >>> 0) < n + 4) return;                          // full: drop (the game's redundancy copes)
  for (let i = 0; i < 4; i++) U8[IN.data + ((h + i) >>> 0) % cap] = (n >>> (8 * i)) & 255;
  for (let i = 0; i < 6; i++) U8[IN.data + ((h + 4 + i) >>> 0) % cap] = src[i];
  for (let i = 0; i < pkt.length; i++) U8[IN.data + ((h + 10 + i) >>> 0) % cap] = pkt[i];
  Atomics.store(H32, IN.head, (h + 4 + n) | 0);
  Atomics.notify(H32, IN.head);
}

// game -> one peer: its data channel here, else the relay; the page's own links are the page's
function sendTo(id, rec) {
  const q = dcs.get(id);
  if (q && q.dc.readyState === 'open') { try { q.dc.send(frame(q.tx, rec.subarray(6))); count(id, 0); return; } catch {} }
  if (peers.get(id)) return;
  if (!open) return;
  const f = rec.slice(); const b = unhex(id); for (let i = 0; i < 6; i++) f[i] = b[i];
  ws.send(f); count(id, 0); count(id, 1);
}
// every record in the out ring
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
    const dst = hexOf(rec);
    if (dst === BCAST) { for (const id of peers.keys()) sendTo(id, rec); }
    else if (peers.has(dst)) sendTo(dst, rec);
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
  if (d.dc) addDc(d.id, d.dc);
  if (d.init) {
    H32 = new Int32Array(d.init.buffer); U8 = new Uint8Array(d.init.buffer);
    OUT = RING(d.init.out); IN = RING(d.init.in);
    OUT.cap = H32[OUT.head + 2] >>> 0; IN.cap = H32[IN.head + 2] >>> 0;
    url = d.init.url; token = d.init.token;
    connect(); loop();
    setInterval(() => { if (stats.size) { postMessage({ stats: Object.fromEntries(stats) }); stats.clear(); } }, 2000);
  }
  if (d.peers) { peers = new Map(d.peers); for (const id of [...dcs.keys()]) if (!peers.has(id)) { try { dcs.get(id).dc.close(); } catch {} dcs.delete(id); } }
  if (d.token) token = d.token;                    // the page resumed its place with a new token
};
