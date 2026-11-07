// Lobby + packet relay for online play. No dependencies.
//
// A room is a virtual LAN. Each member gets a 6-byte node address (the IPX
// node number the game sees). Browsers then try to connect to each other
// directly over WebRTC (signalling goes through here); packets between peers
// that can't connect directly are relayed by this server.
//
// Transports:
//   WebSocket  GET /net            browsers
//   raw TCP    (optional port)     native test builds: frames = u32 len | u8 kind | body
// Messages (kind 0 = JSON text, kind 1 = binary packet):
//   -> {t:'list'}                         <- {t:'rooms', rooms:[...]} now and on every change (room browser)
//   -> {t:'create', room:{name, password, max, listed}, name, build}
//   -> {t:'hello', room, name, password, mustExist, build}
//                                         <- {t:'welcome', id, room, info, peers:[{id,name,state}], token} | {t:'error', code, error}
//      build: the page's game build; a room only takes members of the build it was made with
//      (different builds desync in a race). Names: see cleanName().
//   -> {t:'attach', token}                (a second socket, from the page's network worker: from
//                                         then on this member's binary packets use that socket)
//   -> {t:'leave'}
//   <- {t:'join', id, name} / {t:'leave', id}
//   -> {t:'state', state}                 <- {t:'state', id, state}       ('lobby' | 'game')
//   -> {t:'signal', to, data}             <- {t:'signal', from, data}      (WebRTC)
//   -> {t:'ping', ts, rtt}                <- {t:'pong', ts}                (rtt = the client's last round trip to us)
//   -> {t:'info', data}                   <- {t:'info', from, data}        (to the room: ping tables)
//   -> binary [dst node 6][payload]       <- binary [src node 6][payload]  (dst ff.. = everyone)
import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';
import { createServer as createTcpServer } from 'node:net';
import { appendFileSync, mkdirSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const rooms = new Map();         // code -> Room
const watchers = new Set();      // clients looking at the room browser
const MAX_ROOM = 8;              // NFS4 races take up to 8 players
const BCAST = 'ffffffffffff';
const CODE_ABC = 'abcdefghjkmnpqrstuvwxyz23456789';

// Player names: what the game can show (it stores 10 characters), no look-alike tricks,
// and nothing from a short list of slurs. Empty or rejected -> the page picks Player1234.
export const NAME_MAX = 10;
const BANNED = ['nigg', 'fagg', 'hitler', 'nazi', 'kike', 'retard', 'rape', 'pedo', 'cunt', 'whore', 'chink', 'spic'];
const leet = (s) => s.toLowerCase().replace(/[0@]/g, 'o').replace(/[1!|]/g, 'i').replace(/3/g, 'e').replace(/4/g, 'a').replace(/[5$]/g, 's').replace(/7/g, 't').replace(/[^a-z]/g, '');
export const cleanName = (v) => String(v ?? '').replace(/[^A-Za-z0-9 ._!-]/g, '').replace(/\s+/g, ' ').trim().slice(0, NAME_MAX);
export const badName = (n) => { const l = leet(n); return BANNED.some((b) => l.includes(b)); };
const build = (v) => String(v ?? '').replace(/[^\w.:-]/g, '').slice(0, 80);

// racing time per name (for the start page's board): one line per finished game session
let playLog = null;
export function setPlayLog(dir) { mkdirSync(dir, { recursive: true }); playLog = dir; }
function logPlay(name, secs) {
  if (!playLog || secs < 20) return;
  try { appendFileSync(join(playLog, `play-${new Date().toISOString().slice(0, 7)}.jsonl`), JSON.stringify({ t: Date.now(), n: name, s: Math.round(secs) }) + '\n'); } catch {}
}
const onRoom = new Set();          // listeners for new listed rooms (push notifications)
export const onRoomCreated = (f) => onRoom.add(f);
const tokens = new Map();          // token -> Client, for the network worker's socket

const clean = (v, n) => String(v ?? '').replace(/[\u0000-\u001f\u007f]/g, '').trim().slice(0, n);
const hashPw = (salt, pw) => createHash('sha256').update(salt + '\0' + pw).digest();
function newCode() {
  for (;;) {
    const c = Array.from(randomBytes(6), (b) => CODE_ABC[b % CODE_ABC.length]).join('');
    if (!rooms.has(c)) return c;
  }
}
function newRoom(code, { name, password, max, listed } = {}, owner = '', b = '') {
  const pw = clean(password, 64), salt = randomBytes(8).toString('hex');
  const room = {
    code, name: clean(name, 32) || code, owner, listed: listed !== false, created: Date.now(),
    max: Math.min(MAX_ROOM, Math.max(2, Number(max) || MAX_ROOM)),
    salt, pass: pw ? hashPw(salt, pw) : null, members: new Map(), build: b,
  };
  rooms.set(code, room);
  return room;
}
function newId(room) {
  for (;;) {
    const id = '02' + '00' + randomBytes(4).toString('hex');   // locally administered, never broadcast
    if (!room.members.has(id)) return id;
  }
}
const roomInfo = (r) => ({
  code: r.code, name: r.name, host: r.owner, locked: !!r.pass, max: r.max, created: r.created,
  racing: [...r.members.values()].some((c) => c.state === 'game'),
  players: [...r.members.values()].map((c) => ({ name: c.name, rtt: c.rtt, state: c.state })),
});

// room browser updates: soon after any change, and every few seconds for the pings
let listTimer = null;
function roomsChanged() { if (!listTimer) listTimer = setTimeout(pushRooms, 300); }
function pushRooms() {
  listTimer = null;
  if (!watchers.size) return;
  const msg = JSON.stringify({ t: 'rooms', rooms: [...rooms.values()].filter((r) => r.listed).map(roomInfo) });
  for (const c of watchers) c.sendText(msg);
}
setInterval(pushRooms, 4000).unref();

class Client {
  constructor(sendText, sendBin, close, label) {
    this.sendText = sendText; this.sendBin = sendBin; this.close = close; this.label = label;
    this.id = null; this.room = null; this.name = ''; this.rtt = null; this.state = 'lobby';
    this.token = null; this.gameSince = 0; this.bin = null;
  }
  send(b) { (this.bin || this.sendBin)(b); }
  json(o) { this.sendText(JSON.stringify(o)); }
  fail(code, error) { this.json({ t: 'error', code, error }); }
  enter(room, name) {
    this.room = room; this.name = cleanName(name) || 'Player'; this.id = newId(room); this.state = 'lobby';
    this.token = randomBytes(12).toString('hex'); tokens.set(this.token, this);
    const peers = [...room.members.values()].map((c) => ({ id: c.id, name: c.name, state: c.state }));
    room.members.set(this.id, this);
    if (!room.owner) room.owner = this.name;
    this.json({ t: 'welcome', id: this.id, room: room.code, info: roomInfo(room), peers, token: this.token });
    for (const c of room.members.values()) if (c !== this) c.json({ t: 'join', id: this.id, name: this.name });
    console.log(`[relay] ${this.label} joined ${room.code} "${room.name}" as ${this.id} (${this.name}), ${room.members.size}/${room.max}`);
    roomsChanged();
  }
  onText(s) {
    let m; try { m = JSON.parse(s); } catch { return; }
    if (m.t === 'list') {
      watchers.add(this);
      this.json({ t: 'rooms', rooms: [...rooms.values()].filter((r) => r.listed).map(roomInfo) });
    } else if ((m.t === 'create' || m.t === 'hello') && !this.id && badName(cleanName(m.name))) {
      this.fail('name', 'Please pick another name.');
    } else if (m.t === 'create' && !this.id) {
      const room = newRoom(newCode(), m.room || {}, cleanName(m.name) || 'Player', build(m.build));
      this.enter(room, m.name);
      if (room.listed) for (const f of onRoom) try { f(roomInfo(room)); } catch {}
    } else if (m.t === 'hello' && !this.id) {
      const code = String(m.room || '').toLowerCase().replace(/[^a-z0-9-]/g, '').slice(0, 32);
      if (!code) return this.fail('noroom', 'No room given.');
      let room = rooms.get(code);
      if (!room) {
        if (m.mustExist) return this.fail('gone', 'This room no longer exists.');
        room = newRoom(code, { name: code, listed: false }, '', build(m.build));     // native test builds: RT_ROOM=name
      }
      if (room.build && build(m.build) && room.build !== build(m.build))
        return this.fail('version', 'This room runs another version of the game. Reload the page to update, or ask the room to reload.');
      if (room.members.size >= room.max) return this.fail('full', 'This room is full.');
      if (room.pass) {
        const given = hashPw(room.salt, clean(m.password, 64));
        if (!timingSafeEqual(given, room.pass)) return this.fail('password', m.password ? 'Wrong password.' : 'This room needs a password.');
      }
      this.enter(room, m.name);
    } else if (m.t === 'leave') {
      this.gone();
    } else if (m.t === 'attach') {
      const owner = tokens.get(String(m.token || ''));
      if (owner) { owner.bin = this.sendBin; this.binFor = owner; this.json({ t: 'attached' }); }
      else this.fail('token', 'Unknown token.');
    } else if (m.t === 'state' && this.id) {
      const was = this.state;
      this.state = m.state === 'game' ? 'game' : 'lobby';
      if (this.state === 'game' && was !== 'game') this.gameSince = Date.now();
      if (this.state !== 'game' && was === 'game') { logPlay(this.name, (Date.now() - this.gameSince) / 1000); this.gameSince = 0; }
      for (const c of this.room.members.values()) if (c !== this) c.json({ t: 'state', id: this.id, state: this.state });
      roomsChanged();
    } else if (m.t === 'signal' && this.id) {
      this.room.members.get(m.to)?.json({ t: 'signal', from: this.id, data: m.data });
    } else if (m.t === 'info' && this.id) {
      const s = JSON.stringify({ t: 'info', from: this.id, data: m.data });
      if (s.length < 2048) for (const c of this.room.members.values()) if (c !== this) c.sendText(s);
    } else if (m.t === 'ping') {
      if (Number.isFinite(m.rtt)) this.rtt = Math.round(m.rtt);
      this.json({ t: 'pong', ts: m.ts });
    }
  }
  onBin(buf) {
    if (this.binFor) { this.binFor.onBin(buf); return; }   // the network worker's socket speaks for its page
    if (!this.id || buf.length < 7) return;
    const dst = buf.subarray(0, 6).toString('hex');
    const out = Buffer.allocUnsafe(buf.length);
    Buffer.from(this.id, 'hex').copy(out, 0); buf.copy(out, 6, 6);
    if (dst === BCAST) { for (const c of this.room.members.values()) if (c !== this) c.send(out); }
    else this.room.members.get(dst)?.send(out);
  }
  gone() {
    if (!this.id) return;
    if (this.state === 'game' && this.gameSince) logPlay(this.name, (Date.now() - this.gameSince) / 1000);
    tokens.delete(this.token); this.token = null; this.bin = null;
    const room = this.room;
    room.members.delete(this.id);
    for (const c of room.members.values()) c.json({ t: 'leave', id: this.id });
    if (room.owner === this.name) room.owner = room.members.values().next().value?.name || '';
    if (!room.members.size) rooms.delete(room.code);
    console.log(`[relay] ${this.id} (${this.name}) left ${room.code}`);
    this.id = null; this.room = null;
    roomsChanged();
  }
  closed() {
    watchers.delete(this);
    if (this.binFor) { if (this.binFor.bin === this.sendBin) this.binFor.bin = null; this.binFor = null; }   // back to the page's socket
    this.gone();
  }
}

// ---------------------------------------------------------------- WebSocket
export function handleUpgrade(req, socket) {
  const key = req.headers['sec-websocket-key'];
  if (!key || (req.headers.upgrade || '').toLowerCase() !== 'websocket') { socket.destroy(); return; }
  const accept = createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
               `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  socket.setNoDelay(true);
  const frame = (op, data) => {
    const n = data.length;
    const head = n < 126 ? Buffer.from([0x80 | op, n]) : n < 65536 ? Buffer.from([0x80 | op, 126, n >> 8, n & 255])
      : (() => { const h = Buffer.alloc(10); h[0] = 0x80 | op; h[1] = 127; h.writeBigUInt64BE(BigInt(n), 2); return h; })();
    if (!socket.destroyed) socket.write(Buffer.concat([head, data]));
  };
  const client = new Client((s) => frame(1, Buffer.from(s)), (b) => frame(2, b), () => socket.destroy(),
                            req.socket.remoteAddress);
  let acc = Buffer.alloc(0), frag = [], fragOp = 0;
  socket.on('data', (d) => {
    acc = Buffer.concat([acc, d]);
    for (;;) {
      if (acc.length < 2) return;
      const fin = acc[0] & 0x80, op = acc[0] & 15, masked = acc[1] & 0x80;
      let len = acc[1] & 127, off = 2;
      if (len === 126) { if (acc.length < 4) return; len = acc.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (acc.length < 10) return; len = Number(acc.readBigUInt64BE(2)); off = 10; }
      if (len > 1 << 20) { socket.destroy(); return; }
      const need = off + (masked ? 4 : 0) + len;
      if (acc.length < need) return;
      let payload = acc.subarray(off + (masked ? 4 : 0), need);
      if (masked) { const m = acc.subarray(off, off + 4); payload = Buffer.from(payload); for (let i = 0; i < payload.length; i++) payload[i] ^= m[i & 3]; }
      acc = acc.subarray(need);
      if (op === 8) { socket.end(); return; }
      if (op === 9) { frame(10, payload); continue; }
      if (op === 10) continue;
      if (op) fragOp = op;
      frag.push(payload);
      if (!fin) continue;
      const msg = Buffer.concat(frag); const kind = fragOp; frag = [];
      if (kind === 1) client.onText(msg.toString()); else client.onBin(msg);
    }
  });
  socket.on('close', () => client.closed());
  socket.on('error', () => client.closed());
}

// --------------------------------------------------- raw TCP (native builds)
export function listenRaw(port, host = '127.0.0.1') {
  // port may be a number (TCP on host) or a filesystem path (Unix socket)
  const isPath = typeof port === 'string' && port.includes('/');
  if (isPath) rmSync(port, { force: true });
  createTcpServer((sock) => {
    sock.setNoDelay(true);
    const send = (kind, body) => { const h = Buffer.alloc(5); h.writeUInt32LE(body.length + 1, 0); h[4] = kind; if (!sock.destroyed) sock.write(Buffer.concat([h, body])); };
    const client = new Client((s) => send(0, Buffer.from(s)), (b) => send(1, b), () => sock.destroy(), 'native:' + (sock.remotePort || 'unix'));
    let acc = Buffer.alloc(0);
    sock.on('data', (d) => {
      acc = Buffer.concat([acc, d]);
      while (acc.length >= 4) {
        const n = acc.readUInt32LE(0);
        if (n < 1 || n > 1 << 20) { sock.destroy(); return; }
        if (acc.length < 4 + n) break;
        const kind = acc[4], body = acc.subarray(5, 4 + n); acc = acc.subarray(4 + n);
        if (kind === 0) client.onText(body.toString()); else client.onBin(Buffer.from(body));
      }
    });
    sock.on('close', () => client.closed());
    sock.on('error', () => client.closed());
  }).listen(...(isPath ? [port] : [port, host]), () => console.log(`[relay] native clients: ${isPath ? 'unix:' + port : `tcp://${host}:${port}`}`));
}

export function roomStats() {
  return [...rooms.values()].map(roomInfo);
}

// for the start page (/__live): who is around right now. Names only from listed rooms;
// players in unlisted rooms are counted but not named.
export function liveSummary() {
  let racing = 0, lobby = 0;
  const names = [];
  for (const r of rooms.values()) for (const c of r.members.values()) {
    if (c.state === 'game') racing++; else lobby++;
    if (r.listed) names.push({ n: c.name, racing: c.state === 'game' });
  }
  return { racing, lobby, rooms: [...rooms.values()].filter((r) => r.listed).length, names: names.slice(0, 40) };
}
