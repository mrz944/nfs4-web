// Online play, page side: joins a room on the relay (web/relay.mjs), connects
// to every other member directly with WebRTC data channels, and carries the
// game's IPX packets (runtime/net.c) between them. Packets go peer-to-peer when
// the direct channel is open and through the relay otherwise.
//
// Wire format through the relay: [dst node 6][packet] out, [src node 6][packet] in.
// Data channels are unreliable (no retransmits); instead every message also
// repeats the previous packets (see frame()), so a lost one is recovered from
// the next message ~31 ms later, however long the round trip is.

const ICE = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun.cloudflare.com:3478' }];
const BCAST = 'ffffffffffff';
const hex = (u8) => [...u8].map((b) => b.toString(16).padStart(2, '0')).join('');
const unhex = (s) => Uint8Array.from(s.match(/../g), (h) => parseInt(h, 16));

// ---- redundant framing: [u32 seq of newest][n][ (u16 len, bytes) newest first ... ]
const REDUNDANT = 3, REDUNDANT_BYTES = 1000, REDUNDANT_AGE_MS = 250;
export function frame(tx, pkt) {             // tx = { seq, hist: [{seq, pkt, t}] }
  const now = performance.now();
  tx.seq = (tx.seq + 1) >>> 0;
  tx.hist.unshift({ seq: tx.seq, pkt, t: now });
  if (tx.hist.length > REDUNDANT) tx.hist.length = REDUNDANT;
  const parts = [];
  let size = 5;
  for (const h of tx.hist) {
    if (parts.length && (size + 2 + h.pkt.length > REDUNDANT_BYTES || now - h.t > REDUNDANT_AGE_MS || h.seq !== (tx.seq - parts.length) >>> 0)) break;
    parts.push(h.pkt); size += 2 + h.pkt.length;
  }
  const out = new Uint8Array(size), dv = new DataView(out.buffer);
  dv.setUint32(0, tx.seq, true); out[4] = parts.length;
  let o = 5;
  for (const q of parts) { dv.setUint16(o, q.length, true); out.set(q, o + 2); o += 2 + q.length; }
  return out;
}
// returns the packets not seen before, oldest first; rx = { hi, seen: Set }
export function unframe(rx, m) {
  if (m.length < 5) return [];
  const dv = new DataView(m.buffer, m.byteOffset, m.byteLength);
  const seq = dv.getUint32(0, true), n = m[4], got = [];
  let o = 5;
  for (let i = 0; i < n && o + 2 <= m.length; i++) {
    const len = dv.getUint16(o, true); if (o + 2 + len > m.length) break;
    got.push([(seq - i) >>> 0, m.slice(o + 2, o + 2 + len)]); o += 2 + len;
  }
  const out = [];
  for (const [s, p] of got.reverse()) {
    if (rx.seen.has(s) || (rx.hi != null && s + 512 < rx.hi)) continue;
    rx.seen.add(s); out.push(p);
    if (rx.hi == null || s > rx.hi) {
      if (rx.hi != null && s > rx.hi + 1) rx.gaps += s - rx.hi - 1;   // missing before this one (may be recovered)
      rx.hi = s;
    } else rx.recovered++;
  }
  if (rx.seen.size > 1024) for (const s of rx.seen) { if (s + 512 < rx.hi) rx.seen.delete(s); }
  return out;
}

export class Net extends EventTarget {
  constructor() {
    super();
    this.ws = null; this.id = null; this.room = null; this.info = null;
    this.peers = new Map();      // id -> { name, state, pc, dc, rtt, direct, tx, rx }
    this.mod = null; this.queue = [];
    this.relayRtt = null;
    this.pingTable = new Map();  // id -> { peerId: ms } as reported by each member
    this.fpsTable = new Map();   // id -> frame rate each member's game last ran at
    // last game's frame rate in this browser, so the lobby can weigh it before anyone starts
    try { this.myFps = Number(localStorage.getItem('nfs4.lastFps')) || 0; } catch { this.myFps = 0; }
    this.rooms = [];             // room browser
    this.waiting = null;         // pending create/join: { resolve, reject }
    this.build = '';             // the page's game build: rooms only take members of their own build
    this.token = null;           // lets the network worker's socket speak for us (relay.mjs attach)
    this.useWorker = false;      // packets for relayed peers through net-worker.js (see attach())
    this.worker = null; this.workerOn = false;
  }

  // ------------------------------------------------------------- server
  // one WebSocket for the room browser and for the room we're in
  connect() {
    if (this.ws && this.ws.readyState <= 1) return this.connecting;
    this.connecting = new Promise((resolve, reject) => {
      const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/net`);
      ws.binaryType = 'arraybuffer';
      this.ws = ws;
      const timer = setTimeout(() => { reject(new Error('The online server did not answer.')); ws.close(); }, 8000);
      ws.onopen = () => {
        clearTimeout(timer);
        this.send({ t: 'list' });
        clearInterval(this.pingTimer);
        this.pingTimer = setInterval(() => this.send({ t: 'ping', ts: performance.now(), rtt: this.relayRtt ?? undefined }), 2000);
        this.send({ t: 'ping', ts: performance.now() });
        resolve();
      };
      ws.onerror = () => { clearTimeout(timer); reject(new Error('Could not reach the online server.')); };
      ws.onclose = () => {
        clearInterval(this.pingTimer);
        if (this.id) this.emit('log', 'disconnected from the online server');
        this.dropRoom();
        this.waiting?.reject(new Error('Disconnected from the online server.')); this.waiting = null;
        this.emit('disconnected');
      };
      ws.onmessage = (ev) => {
        if (typeof ev.data !== 'string') { this.fromRelay(new Uint8Array(ev.data)); return; }
        this.onMessage(JSON.parse(ev.data));
      };
    });
    return this.connecting;
  }
  onMessage(m) {
    if (m.t === 'rooms') { this.rooms = m.rooms; this.emit('rooms', m.rooms); }
    else if (m.t === 'welcome') {
      this.id = m.id; this.room = m.room; this.info = m.info; this.token = m.token;
      for (const p of m.peers) this.addPeer(p.id, p.name, true, p.state);   // we are the newcomer: we call them
      this.emit('status');
      this.waiting?.resolve(m); this.waiting = null;
    } else if (m.t === 'error') { const e = new Error(m.error); e.code = m.code; this.waiting?.reject(e); this.waiting = null; }
    else if (m.t === 'join') { this.addPeer(m.id, m.name, false); this.emit('log', `${m.name} joined the room`); this.emit('status'); }
    else if (m.t === 'leave') { const p = this.peers.get(m.id); if (p) { this.closePeer(m.id); this.emit('log', `${p.name} left`); this.emit('status'); } }
    else if (m.t === 'state') { const p = this.peers.get(m.id); if (p) { p.state = m.state; this.emit('status'); } }
    else if (m.t === 'signal') this.onSignal(m.from, m.data);
    else if (m.t === 'pong') { this.relayRtt = performance.now() - m.ts; if (this.id) this.shareRtts(); this.emit('status'); }
    else if (m.t === 'info' && m.data?.rtt) { this.pingTable.set(m.from, m.data.rtt); if (m.data.fps) this.fpsTable.set(m.from, m.data.fps); this.emit('status'); }
  }
  request(msg) {
    if (this.id) return Promise.reject(new Error('Already in a room.'));
    return this.connect().then(() => new Promise((resolve, reject) => {
      this.waiting = { resolve, reject };
      this.send(msg);
      setTimeout(() => { if (this.waiting?.resolve === resolve) { this.waiting = null; reject(new Error('The online server did not answer.')); } }, 8000);
    }));
  }
  create(room, name) { return this.request({ t: 'create', room, name, build: this.build }); }
  join(code, name, password) { return this.request({ t: 'hello', room: code, name, password, mustExist: true, build: this.build }); }
  leave() { this.send({ t: 'leave' }); this.dropRoom(); this.emit('status'); }
  setState(state) { this.state = state; this.send({ t: 'state', state }); }
  dropRoom() {
    for (const id of [...this.peers.keys()]) this.closePeer(id);
    this.id = null; this.room = null; this.info = null; this.pingTable.clear(); this.queue = [];
  }
  closePeer(id) {
    const p = this.peers.get(id); if (!p) return;
    clearInterval(p.statsTimer); try { p.pc.close(); } catch {}
    this.peers.delete(id); this.pingTable.delete(id);
    this.postPeers();
  }
  send(o) { if (this.ws?.readyState === 1) this.ws.send(JSON.stringify(o)); }
  emit(type, detail) { this.dispatchEvent(new CustomEvent(type, { detail })); }

  // ------------------------------------------------------------ WebRTC
  addPeer(id, name, initiator, state = 'lobby') {
    const p = { name, state, pc: new RTCPeerConnection({ iceServers: ICE }), dc: null, rtt: null, direct: false,
                tx: { seq: 0, hist: [] }, rx: { hi: null, seen: new Set(), gaps: 0, recovered: 0 },
                ctr: { tx: 0, txRelay: 0, rx: 0, rxRelay: 0, last: 0, maxGap: 0, gaps: 0, recovered: 0 } };
    this.peers.set(id, p);
    this.postPeers();
    p.pc.onicecandidate = (e) => { if (e.candidate) this.send({ t: 'signal', to: id, data: { ice: e.candidate } }); };
    p.pc.onconnectionstatechange = () => { if (p.pc.connectionState === 'failed') this.emit('log', `no direct connection to ${name}, using the relay`); this.emit('status'); };
    const setup = (dc) => {
      p.dc = dc; dc.binaryType = 'arraybuffer';
      dc.onopen = () => { p.direct = true; this.postPeers(); this.emit('log', `direct connection to ${name}`); this.emit('status'); };
      dc.onclose = () => { p.direct = false; this.postPeers(); this.emit('status'); };
      dc.onmessage = (ev) => { for (const pkt of unframe(p.rx, new Uint8Array(ev.data))) this.deliver(id, pkt); };
    };
    if (initiator) {
      // UDP-like: unordered, never retransmitted (losses are covered by the redundant framing)
      setup(p.pc.createDataChannel('ipx', { ordered: false, maxRetransmits: 0 }));
      p.pc.createOffer().then((o) => p.pc.setLocalDescription(o)).then(() => this.send({ t: 'signal', to: id, data: { sdp: p.pc.localDescription } }));
    } else {
      p.pc.ondatachannel = (e) => setup(e.channel);
    }
    p.statsTimer = setInterval(async () => {
      if (!p.direct) return;
      const stats = await p.pc.getStats();
      stats.forEach((s) => { if (s.type === 'candidate-pair' && s.nominated && s.currentRoundTripTime != null) p.rtt = s.currentRoundTripTime * 1000; });
      this.emit('status');
    }, 2000);
  }
  async onSignal(from, data) {
    let p = this.peers.get(from);
    if (!p) return;
    if (data.sdp) {
      await p.pc.setRemoteDescription(data.sdp);
      if (data.sdp.type === 'offer') {
        await p.pc.setLocalDescription(await p.pc.createAnswer());
        this.send({ t: 'signal', to: from, data: { sdp: p.pc.localDescription } });
      }
    } else if (data.ice) { try { await p.pc.addIceCandidate(data.ice); } catch {} }
  }

  // ----------------------------------------------------------- packets
  attach(mod) {
    this.mod = mod;
    // called (on this thread) by runtime/net.c for every packet the game sends: [dst 6][packet]
    mod.netSend = (ptr, len) => {
      const buf = mod.HEAPU8.slice(ptr, ptr + len); mod._free(ptr);
      const dst = hex(buf.subarray(0, 6)), pkt = buf.subarray(6);
      if (dst === BCAST) { for (const id of this.peers.keys()) this.sendTo(id, pkt); }
      else this.sendTo(dst, pkt);
    };
    for (const [src, pkt] of this.queue) this.deliver(src, pkt);
    this.queue = [];
    if (this.useWorker && this.token) this.startWorker(mod);
  }
  // The network worker (net-worker.js) takes the packets for peers without a direct link off
  // the main thread: runtime/net.c also writes every packet into a shared ring, the worker sends
  // the relayed ones over its own socket and puts incoming ones straight into the game's ring.
  // If it can't attach (or its socket drops), the main thread carries everything as before.
  startWorker(mod) {
    const a = mod._rt_net_worker(1), dv = new DataView(mod.HEAPU8.buffer);
    const out = dv.getUint32(a, true), inn = dv.getUint32(a + 4, true);
    const w = this.worker = new Worker(new URL('net-worker.js', import.meta.url));
    const off = (why) => { if (this.workerOn) this.emit('log', `network worker off (${why}), packets go through the page`); this.workerOn = false; mod._rt_net_worker(0); };
    const fallback = setTimeout(() => { if (!this.workerOn) off('no answer'); }, 4000);
    w.onmessage = (e) => {
      const d = e.data;
      if (d.attached) { clearTimeout(fallback); this.workerOn = true; mod._rt_net_worker(1); this.postPeers(); this.emit('log', 'network worker on: relayed packets bypass the main thread'); }
      else if (d.closed) off('socket closed');
      else if (d.error) off(d.error);
      else if (d.stats) for (const [id, [tx, rx]] of Object.entries(d.stats)) { const c = this.peers.get(id)?.ctr; if (c) { c.tx += tx; c.txRelay += tx; c.rx += rx; c.rxRelay += rx; c.last = performance.now(); } }
    };
    w.onerror = (e) => off('error ' + (e.message || ''));
    const wsUrl = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/net`;
    w.postMessage({ init: { buffer: mod.HEAPU8.buffer, out, in: inn, url: wsUrl, token: this.token } });
  }
  postPeers() { this.worker?.postMessage({ peers: [...this.peers].map(([id, p]) => [id, p.dc?.readyState === 'open']) }); }
  sendTo(id, pkt) {
    const p = this.peers.get(id);
    if (this.workerOn && p?.dc?.readyState !== 'open') return;           // the worker sends this one
    if (p) p.ctr.tx++;
    if (p?.dc?.readyState === 'open') { try { p.dc.send(frame(p.tx, pkt)); return; } catch {} }
    if (p) p.ctr.txRelay++;
    if (this.ws?.readyState === 1) { const f = new Uint8Array(6 + pkt.length); f.set(unhex(id)); f.set(pkt, 6); this.ws.send(f); }
  }
  fromRelay(f) { if (f.length > 6) this.deliver(hex(f.subarray(0, 6)), f.subarray(6), true); }
  deliver(srcId, pkt, relayed = false) {
    const mod = this.mod;
    const c = this.peers.get(srcId)?.ctr;
    if (c) {
      const now = performance.now();
      c.rx++; if (relayed) c.rxRelay++;
      if (c.last) c.maxGap = Math.max(c.maxGap, now - c.last);
      c.last = now;
    }
    if (!mod) { if (this.queue.length < 256) this.queue.push([srcId, pkt]); return; }
    const n = 6 + pkt.length, p = mod._malloc(n);
    mod.HEAPU8.set(unhex(srcId), p); mod.HEAPU8.set(pkt, p + 6);
    mod._rt_net_packet(p, n);
    mod._free(p);
  }

  // round trip to a peer: measured on the direct link, else estimated through the relay
  rttTo(p) { return p.direct && p.rtt != null ? p.rtt : this.relayRtt != null ? this.relayRtt * 2 : null; }
  shareRtts() {
    const rtt = {};
    for (const [id, p] of this.peers) { const r = this.rttTo(p); if (r != null) rtt[id] = Math.round(r); }
    this.pingTable.set(this.id, rtt);
    if (this.myFps) this.fpsTable.set(this.id, this.myFps);
    if (this.peers.size) this.send({ t: 'info', data: { rtt, fps: this.myFps || undefined } });
  }
  // The host's input counts at once, a client's after one round trip to the host,
  // so the best host is the member whose worst round trip to anyone is smallest.
  // The host also paces the race for everyone, so a host that can't hold 60 fps costs
  // each player its extra frame time (several times over: it delays every tick).
  hostCost(id) {
    const fps = this.fpsTable.get(id);
    return fps ? Math.max(0, 1000 / fps - 1000 / 60) * 4 : 0;
  }
  bestHost() {
    if (!this.peers.size) return null;
    const ids = [this.id, ...this.peers.keys()];
    let best = null;
    for (const a of ids) {
      let worst = 0;
      for (const b of ids) if (a !== b) {
        const r = this.pingTable.get(a)?.[b] ?? this.pingTable.get(b)?.[a];
        if (r == null) { worst = Infinity; break; }
        worst = Math.max(worst, r);
      }
      if (worst < Infinity) worst += this.hostCost(a);
      if (worst < Infinity && (!best || worst < best.worst)) best = { id: a, worst };
    }
    if (!best) return null;
    return { me: best.id === this.id, name: best.id === this.id ? 'you' : this.peers.get(best.id)?.name, worst: Math.round(best.worst) };
  }
  // one line per peer for the lag log (main.js, every 2 s while racing online); resets the counters
  netStats() {
    return [...this.peers.values()].map((p) => {
      const c = p.ctr, r = this.rttTo(p);
      const lost = Math.max(0, p.rx.gaps - p.rx.recovered) - Math.max(0, c.gaps - c.recovered);
      const line = `${p.name}: ${p.direct ? 'direct' : 'RELAY'} rtt=${r != null ? Math.round(r) : '?'} relay=${this.relayRtt != null ? Math.round(this.relayRtt) : '?'}`
        + ` tx=${c.tx}${c.txRelay ? `(${c.txRelay} relay)` : ''} rx=${c.rx}${c.rxRelay ? `(${c.rxRelay} relay)` : ''}`
        + ` lost=${lost} recovered=${p.rx.recovered - c.recovered} maxgap=${Math.round(c.maxGap)}ms state=${p.state}`
        + (c.last && performance.now() - c.last > 1000 ? ` silent=${Math.round(performance.now() - c.last)}ms` : '');
      Object.assign(c, { tx: 0, txRelay: 0, rx: 0, rxRelay: 0, maxGap: 0, gaps: p.rx.gaps, recovered: p.rx.recovered });
      return line;
    });
  }
  summary() {
    const list = [...this.peers.values()].map((p) => {
      const r = this.rttTo(p), loss = p.rx.hi ? Math.round(100 * Math.max(0, p.rx.gaps - p.rx.recovered) / p.rx.hi) : 0;
      return `${p.name}: ${r != null ? Math.round(r) + ' ms ' : ''}${p.direct ? 'direct' : 'relay'}${loss ? ` ${loss}% lost` : ''}`;
    });
    return { room: this.room, name: this.info?.name, players: this.peers.size + 1, peers: list, best: this.bestHost() };
  }
}

