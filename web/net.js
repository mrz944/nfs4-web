// Online play, page side: joins a room on the relay (web/relay.mjs), connects
// to every other member directly with WebRTC data channels, and carries the
// game's IPX packets (runtime/net.c) between them. Packets go peer-to-peer when
// the direct channel is open and through the relay otherwise.
//
// Wire format through the relay: [dst node 6][packet] out, [src node 6][packet] in.
// Data channels are unreliable (no retransmits); instead every message also
// repeats the previous packets (see frame()), so a lost one is recovered from
// the next message ~31 ms later, however long the round trip is.

// STUN only, until the server answers /__ice (serve.mjs): with a TURN key there, TURN relays on
// Cloudflare's edge as well, so two players behind strict routers still get a UDP link instead
// of the relay through the server
const ICE = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun.cloudflare.com:3478' }];
const BCAST = 'ffffffffffff';
const hex = (u8) => [...u8].map((b) => b.toString(16).padStart(2, '0')).join('');
const unhex = (s) => Uint8Array.from(s.match(/../g), (h) => parseInt(h, 16));
// for the link log: a candidate as "relay/udp" (its type and protocol, never its address)
const candType = (c) => { const s = c?.candidate || ''; const t = / typ (\w+)/.exec(s)?.[1]; return t ? `${t}/${/ (udp|tcp) /i.exec(s)?.[1].toLowerCase() || '?'}` : null; };
const count = (o, k) => { if (k) o[k] = (o[k] || 0) + 1; };
const tally = (o) => Object.entries(o).map(([k, n]) => `${k}x${n}`).join(' ') || 'none';
const pairSide = (c) => !c ? '?' : `${c.candidateType}/${c.protocol}${c.relayProtocol ? `(turn ${c.relayProtocol})` : ''}${c.networkType && c.networkType !== 'unknown' ? ` ${c.networkType}` : ''}`;

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
    this.useWorker = false;      // the network worker (net-worker.js) carries the packets, see startWorker()
    this.worker = null; this.workerOn = false; this.workerRelay = false; this.canTransfer = null;
    this.ice = ICE; this.iceAt = 0; this.turn = false;
    this.player = null;          // () => Promise<{k, s}>: the page's player key (players.mjs), sent with create/hello
  }
  // the ICE servers for new links, refreshed now and then (the TURN credentials are short-lived)
  fetchIce() {
    if (Date.now() - this.iceAt < 3600e3) return;
    this.iceAt = Date.now();
    this.iceReady = fetch('__ice', { cache: 'no-store' }).then((r) => r.json()).then((j) => {
      if (Array.isArray(j.iceServers) && j.iceServers.length) { this.ice = j.iceServers; if (j.turn && !this.turn) this.emit('log', 'TURN relays available for direct links'); this.turn = !!j.turn; }
    }).catch(() => {});
  }
  // before joining a room: the links to its members are made at once, so wait (briefly) for the
  // TURN servers, or those links would try STUN only
  iceWait() { return Promise.race([this.iceReady, new Promise((r) => setTimeout(r, 2000))]); }

  // ------------------------------------------------------------- server
  // one WebSocket for the room browser and for the room we're in
  connect() {
    if (this.ws && this.ws.readyState <= 1) return this.connecting;
    this.fetchIce();
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
        clearTimeout(timer);
        clearInterval(this.pingTimer);
        if (this.ws !== ws) return;
        // in a room: reconnect and take our place back (relay.mjs keeps it for 20 s, and the
        // others never see us leave); direct links to the others carry on meanwhile
        if (this.id && this.token && !this.waiting) { this.reconnect(); return; }
        if (this.id) this.emit('log', 'disconnected from the online server');
        this.lostAt = 0;
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
  // the socket dropped while in a room: try again (quickly at first) until the server would have
  // given our place away, then give up as before
  reconnect() {
    const now = Date.now();
    if (!this.lostAt) { this.lostAt = now; this.tries = 0; this.emit('log', 'lost the connection to the online server, reconnecting'); this.emit('status'); }
    if (now - this.lostAt > 18000) {
      this.lostAt = 0; this.ws = null;
      this.emit('log', 'disconnected from the online server');
      this.dropRoom(); this.emit('disconnected'); return;
    }
    const wait = [100, 400, 1000, 2000][this.tries++] ?? 3000;
    setTimeout(() => {
      if (!this.id || !this.lostAt) return;
      // a failed try closes, and comes back here; so does no answer (a server without resume)
      this.connect().then(() => {
        const ws = this.ws;
        this.send({ t: 'resume', token: this.token });
        clearTimeout(this.resumeTimer);
        this.resumeTimer = setTimeout(() => { if (this.lostAt && this.ws === ws) ws.close(); }, 4000);
      }).catch(() => {});
    }, wait);
  }
  onResumed(m) {
    clearTimeout(this.resumeTimer);
    const away = Date.now() - this.lostAt;
    this.lostAt = 0; this.token = m.token; this.info = m.info;
    this.worker?.postMessage({ token: m.token });
    // whoever came or went meanwhile (their messages to us were lost): new ones go through the
    // relay until they call us again, gone ones are dropped
    const now = new Map(m.peers.map((p) => [p.id, p]));
    for (const id of [...this.peers.keys()]) if (!now.has(id)) { this.emit('log', `${this.peers.get(id).name} left`); this.closePeer(id); }
    for (const p of m.peers) {
      const q = this.peers.get(p.id);
      if (q) q.state = p.state; else { this.addPeer(p.id, p.name, false, p.state, p.cc); this.emit('log', `${p.name} joined the room`); }
    }
    if (this.state) this.send({ t: 'state', state: this.state });
    this.emit('log', `reconnected to the online server after ${(away / 1000).toFixed(1)} s`);
    this.emit('status');
  }
  onMessage(m) {
    if (m.t === 'rooms') { this.rooms = m.rooms; this.emit('rooms', m.rooms); }
    else if (m.t === 'welcome' && m.resumed) this.onResumed(m);
    else if (m.t === 'welcome') {
      this.id = m.id; this.room = m.room; this.info = m.info; this.token = m.token; this.cc = m.cc || '';
      for (const p of m.peers) this.addPeer(p.id, p.name, true, p.state, p.cc);   // we are the newcomer: we call them
      this.emit('status');
      this.waiting?.resolve(m); this.waiting = null;
    } else if (m.t === 'error' && m.code === 'resume') {
      clearTimeout(this.resumeTimer); this.lostAt = 0; this.emit('log', 'disconnected from the online server (our place in the room was gone)');
      this.dropRoom(); this.emit('disconnected');
    } else if (m.t === 'error') { const e = new Error(m.error); e.code = m.code; this.waiting?.reject(e); this.waiting = null; }
    else if (m.t === 'join') { this.addPeer(m.id, m.name, false, 'lobby', m.cc); this.emit('log', `${m.name} joined the room`); this.emit('status'); }
    else if (m.t === 'leave') { const p = this.peers.get(m.id); if (p) { this.closePeer(m.id); this.emit('log', `${p.name} left`); this.emit('status'); } }
    else if (m.t === 'state') { const p = this.peers.get(m.id); if (p) { p.state = m.state; this.emit('status'); } }
    else if (m.t === 'signal') this.onSignal(m.from, m.data);
    else if (m.t === 'pong') { this.relayRtt = performance.now() - m.ts; if (this.id) this.shareRtts(); this.emit('status'); }
    else if (m.t === 'info' && m.data?.rtt) {
      this.pingTable.set(m.from, m.data.rtt); if (m.data.fps) this.fpsTable.set(m.from, m.data.fps);
      const p = this.peers.get(m.from); if (p && Number.isFinite(m.data.relay)) p.relayRtt = m.data.relay;
      this.emit('status');
    }
  }
  request(msg) {
    if (this.id) return Promise.reject(new Error('Already in a room.'));
    return this.connect().then(() => Promise.all([this.iceWait(), this.player?.()])).then(([, who]) => new Promise((resolve, reject) => {
      this.waiting = { resolve, reject };
      this.send(who ? { ...msg, ...who } : msg);
      setTimeout(() => { if (this.waiting?.resolve === resolve) { this.waiting = null; reject(new Error('The online server did not answer.')); } }, 8000);
    }));
  }
  create(room, name) { return this.request({ t: 'create', room, name, build: this.build }); }
  join(code, name, password) { return this.request({ t: 'hello', room: code, name, password, mustExist: true, build: this.build }); }
  leave() { this.send({ t: 'leave' }); this.lostAt = 0; this.dropRoom(); this.emit('status'); }
  setState(state) { this.state = state; this.send({ t: 'state', state }); }
  dropRoom() {
    for (const id of [...this.peers.keys()]) this.closePeer(id);
    this.id = null; this.room = null; this.info = null; this.pingTable.clear(); this.queue = [];
  }
  closePeer(id) {
    const p = this.peers.get(id); if (!p) return;
    clearInterval(p.statsTimer); clearTimeout(p.iceTimer); try { p.pc.close(); } catch {}
    this.peers.delete(id); this.pingTable.delete(id);
    this.postPeers();
  }
  send(o) { if (this.ws?.readyState === 1) this.ws.send(JSON.stringify(o)); }
  emit(type, detail) { this.dispatchEvent(new CustomEvent(type, { detail })); }

  // ------------------------------------------------------------ WebRTC
  addPeer(id, name, initiator, state = 'lobby', cc = '') {
    this.fetchIce();
    const p = { name, state, cc, pc: new RTCPeerConnection({ iceServers: this.ice }), dc: null, rtt: null, direct: false, turn: false,
                tx: { seq: 0, hist: [] }, rx: { hi: null, seen: new Set(), gaps: 0, recovered: 0 },
                ctr: { tx: 0, txRelay: 0, rx: 0, rxRelay: 0, last: 0, maxGap: 0, gaps: 0, recovered: 0 } };
    this.peers.set(id, p);
    this.postPeers();
    // what ICE had to work with, for the log when a link fails (candidate types only, no addresses)
    p.diag = { loc: {}, rem: {}, errs: [], turn: this.ice.some((s) => [].concat(s.urls).some((u) => /^turns?:/.test(u))) };
    p.pc.onicecandidate = (e) => { if (e.candidate) { count(p.diag.loc, candType(e.candidate)); this.send({ t: 'signal', to: id, data: { ice: e.candidate } }); } };
    p.pc.onicecandidateerror = (e) => {
      if (p.diag.errs.length < 6) p.diag.errs.push(`${e.errorCode} ${(e.url || '').replace('?transport=', '/')} ${e.errorText || ''}`.trim());
    };
    p.pc.onconnectionstatechange = () => {
      if (p.pc.connectionState === 'failed') { this.emit('log', `no direct connection to ${name}, using the relay`); this.iceReport(p); }
      this.emit('status');
    };
    // some links never fail, they just never come up (no candidates of our own): report those too
    p.iceTimer = setTimeout(() => { if (!p.direct && this.peers.get(id) === p) this.iceReport(p, 'not up after 20 s'); }, 20000);
    const setup = (dc) => {
      if (this.giveToWorker(id, dc)) { p.inWorker = true; return; }
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
      let pair = null;
      stats.forEach((s) => { if (s.type === 'candidate-pair' && s.nominated && s.currentRoundTripTime != null) { p.rtt = s.currentRoundTripTime * 1000; pair = s; } });
      if (pair) {
        const loc = stats.get(pair.localCandidateId), rem = stats.get(pair.remoteCandidateId);
        p.turn = loc?.candidateType === 'relay' || rem?.candidateType === 'relay';
        if (!p.pairLogged) { p.pairLogged = true; this.emit('log', `link to ${name}: ${pairSide(loc)} <-> ${pairSide(rem)}`); }
      }
      this.emit('status');
    }, 2000);
  }
  // each ICE server URL on its own: the candidates it gives in 10 s and the errors (no addresses,
  // no credentials in the log)
  async probeIce() {
    const urls = [];
    for (const s of [...this.ice, ...ICE]) for (const u of [].concat(s.urls)) {
      if (!urls.some((x) => x.urls === u)) urls.push({ urls: u, username: s.username, credential: s.credential });
    }
    this.emit('log', `ice probe: ${urls.length} server URLs, ${this.turn ? 'with' : 'without'} TURN`);
    const one = (srv) => new Promise((resolve) => {
      const relay = /^turns?:/.test(srv.urls), t0 = performance.now(), got = {}, errs = [];
      let pc;
      try { pc = new RTCPeerConnection({ iceServers: [srv], iceTransportPolicy: relay ? 'relay' : 'all' }); }
      catch (e) { resolve(`refused (${e.name}: ${e.message})`); return; }
      let first = null;
      pc.onicecandidate = (e) => { const t = candType(e.candidate); if (t) { count(got, t); if (first == null && (relay ? /^relay/ : /^srflx/).test(t)) first = performance.now() - t0; } };
      pc.onicecandidateerror = (e) => { if (errs.length < 3) errs.push(`${e.errorCode} ${e.errorText || ''}`.trim()); };
      const end = () => {
        clearTimeout(timer); const state = pc.iceGatheringState; pc.close();
        resolve(`${tally(got)}${first != null ? ` (first in ${Math.round(first)} ms)` : ''} gathering=${state}${errs.length ? ` errors ${errs.join('; ')}` : ''}`);
      };
      const timer = setTimeout(end, 10000);
      pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === 'complete') end(); };
      pc.createDataChannel('probe');
      pc.createOffer().then((o) => pc.setLocalDescription(o)).catch((e) => { errs.push(`offer: ${e.name}`); end(); });
    });
    const res = await Promise.all(urls.map(one));
    urls.forEach((s, i) => this.emit('log', `ice probe ${s.urls.replace('?transport=', '/')}: ${res[i]}`));
  }
  // one line about a failed link: whether TURN was offered, the candidates each side gathered,
  // how the candidate pairs ended, and the TURN servers' errors
  iceReport(p, why = 'failed') {
    if (p.reported) return;
    p.reported = true;
    const d = p.diag, pc = p.pc;
    p.pc.getStats().then((stats) => {
      const pairs = {};
      stats.forEach((s) => {
        if (s.type !== 'candidate-pair') return;
        const l = stats.get(s.localCandidateId), r = stats.get(s.remoteCandidateId);
        count(pairs, `${l?.candidateType || '?'}-${r?.candidateType || '?'}:${s.state}`);
      });
      this.emit('log', `ice to ${p.name} (${why}): turn=${d.turn ? 'yes' : 'no'} gathering=${pc.iceGatheringState} ice=${pc.iceConnectionState}`
        + ` signaling=${pc.signalingState} | ours ${tally(d.loc)} | theirs ${tally(d.rem)} | pairs ${tally(pairs)}`
        + (d.errs.length ? ` | errors ${d.errs.join('; ')}` : ''));
    }).catch((e) => this.emit('log', `ice to ${p.name}: no stats (${e.message})`));
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
    } else if (data.ice) {
      count(p.diag.rem, candType(data.ice));
      try { await p.pc.addIceCandidate(data.ice); } catch (e) { if (p.diag.errs.length < 6) p.diag.errs.push(`theirs refused: ${e.name}`); }
    }
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
  // The data channels go to the worker as soon as they are made (while still fresh: a used
  // channel can't be transferred), so every packet of the game, direct or relayed, is sent and
  // received off this thread. Browsers without transferable channels keep them here.
  giveToWorker(id, dc) {
    if (!this.useWorker || this.canTransfer === false) return false;
    const w = this.getWorker();
    try { w.postMessage({ dc, id }, [dc]); this.canTransfer = true; return true; }
    catch (e) { if (this.canTransfer == null) this.emit('log', `data channels stay on the page (${e.name || e.message})`); this.canTransfer = false; return false; }
  }
  getWorker() {
    if (this.worker) return this.worker;
    const w = this.worker = new Worker(new URL('net-worker.js', import.meta.url), { type: 'module' });
    w.onmessage = (e) => {
      const d = e.data;
      if (d.dcOpen) { const p = this.peers.get(d.dcOpen); if (p) { p.direct = true; this.postPeers(); this.emit('log', `direct connection to ${p.name}`); this.emit('status'); } }
      else if (d.dcClosed) { const p = this.peers.get(d.dcClosed); if (p) { p.direct = false; this.postPeers(); this.emit('status'); } }
      else if (d.attached) { this.workerRelay = true; this.postPeers(); this.emit('log', 'network worker on: packets bypass the main thread'); }
      else if (d.closed) { this.workerRelay = false; this.emit('log', 'network worker: relay socket closed, the page relays meanwhile'); }
      else if (d.error) { this.workerRelay = false; this.emit('log', `network worker: relay socket refused (${d.error}), the page relays`); }
      else if (d.stats) for (const [id, [tx, txRelay, rx, rxRelay, gaps, recovered]] of Object.entries(d.stats)) { const p = this.peers.get(id); if (p) { const c = p.ctr; c.tx += tx; c.txRelay += txRelay; c.rx += rx; c.rxRelay += rxRelay; p.rx.gaps += gaps || 0; p.rx.recovered += recovered || 0; p.rx.hi = (p.rx.hi || 0) + (rx - rxRelay); if (rx) c.last = performance.now(); } }
    };
    w.onerror = (e) => { this.workerRelay = false; this.emit('log', 'network worker error ' + (e.message || '')); };
    return w;
  }
  // The network worker (net-worker.js) takes the packets for peers without a direct link off
  // the main thread: runtime/net.c also writes every packet into a shared ring, the worker sends
  // the relayed ones over its own socket and puts incoming ones straight into the game's ring.
  // If it can't attach (or its socket drops), the main thread carries everything as before.
  startWorker(mod) {
    const a = mod._rt_net_worker(1), dv = new DataView(mod.HEAPU8.buffer);
    const out = dv.getUint32(a, true), inn = dv.getUint32(a + 4, true);
    const w = this.getWorker();
    this.workerOn = true;                                            // the rings are up: the worker carries what it can
    const wsUrl = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/net`;
    w.postMessage({ init: { buffer: mod.HEAPU8.buffer, out, in: inn, url: wsUrl, token: this.token } });
    this.postPeers();
  }
  postPeers() { this.worker?.postMessage({ peers: [...this.peers].map(([id, p]) => [id, !p.inWorker && p.dc?.readyState === 'open']) }); }
  sendTo(id, pkt) {
    const p = this.peers.get(id);
    // the worker's: its data channel, or the relay through its socket; the page relays only
    // while the worker's socket is down
    if (this.workerOn && p?.inWorker && (p.direct || this.workerRelay)) return;
    if (this.workerOn && !p?.inWorker && p?.dc?.readyState !== 'open' && this.workerRelay) return;
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

  // round trip to a peer: measured on the direct link, else through the relay (our way to the
  // server and theirs, which each member shares; ours twice until theirs is known)
  rttTo(p) {
    if (p.direct && p.rtt != null) return p.rtt;
    return this.relayRtt != null ? this.relayRtt + (p.relayRtt ?? this.relayRtt) : null;
  }
  shareRtts() {
    const rtt = {};
    for (const [id, p] of this.peers) { const r = this.rttTo(p); if (r != null) rtt[id] = Math.round(r); }
    this.pingTable.set(this.id, rtt);
    if (this.myFps) this.fpsTable.set(this.id, this.myFps);
    if (this.peers.size) this.send({ t: 'info', data: { rtt, fps: this.myFps || undefined, relay: this.relayRtt != null ? Math.round(this.relayRtt) : undefined } });
    // and to the game, which moves the others' cars ahead by how old their updates are (net.c)
    const mod = this.mod;
    if (mod?._rt_net_peer_rtt) for (const [id, r] of Object.entries(rtt)) {
      const p = mod._malloc(13);
      for (let i = 0; i < 12; i++) mod.HEAPU8[p + i] = id.charCodeAt(i);
      mod.HEAPU8[p + 12] = 0;
      mod._rt_net_peer_rtt(p, r); mod._free(p);
    }
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
      const line = `${p.name}: ${p.direct ? (p.turn ? 'TURN' : 'direct') : 'RELAY'} rtt=${r != null ? Math.round(r) : '?'} relay=${this.relayRtt != null ? Math.round(this.relayRtt) : '?'}`
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
      return `${p.name}: ${r != null ? Math.round(r) + ' ms ' : ''}${p.direct ? (p.turn ? 'turn' : 'direct') : 'relay'}${loss ? ` ${loss}% lost` : ''}`;
    });
    return { room: this.room, name: this.info?.name, players: this.peers.size + 1, peers: list, best: this.bestHost() };
  }
}

