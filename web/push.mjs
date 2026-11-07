// Web Push for the start page's "Notify me" button. No dependencies: VAPID (RFC 8292) and
// message encryption (aes128gcm, RFC 8291) are done with node:crypto.
//
// Keys and subscriptions live in <dir>/vapid.json and <dir>/subs.json (build/push/, never
// committed). Pushes go out when someone opens a listed room (at most one per 10 minutes)
// and for the daily race (--daily HH:MM in serve.mjs): 10 minutes before and at the start.
import { createCipheriv, createECDH, createPrivateKey, generateKeyPairSync, hkdfSync, randomBytes, sign } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const b64u = (b) => Buffer.from(b).toString('base64url');
// only real browser push services: the server POSTs to these URLs
const PUSH_HOSTS = /^(fcm\.googleapis\.com|android\.googleapis\.com|updates\.push\.services\.mozilla\.com|[\w-]+\.push\.apple\.com|[\w.-]+\.notify\.windows\.com)$/;

export function createPush(dir, { subject = 'https://nfs4.xyz/' } = {}) {
  mkdirSync(dir, { recursive: true });
  const keyFile = join(dir, 'vapid.json'), subFile = join(dir, 'subs.json');
  let vapid;
  try { vapid = JSON.parse(readFileSync(keyFile, 'utf8')); } catch {
    const { privateKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
    const jwk = privateKey.export({ format: 'jwk' });
    vapid = { jwk, publicKey: b64u(Buffer.concat([Buffer.from([4]), Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')])) };
    writeFileSync(keyFile, JSON.stringify(vapid), { mode: 0o600 });
  }
  const priv = createPrivateKey({ key: vapid.jwk, format: 'jwk' });
  let subs = [];
  try { subs = JSON.parse(readFileSync(subFile, 'utf8')); } catch {}
  const save = () => writeFileSync(subFile, JSON.stringify(subs));

  function valid(s) {
    try {
      const u = new URL(s.endpoint);
      return u.protocol === 'https:' && PUSH_HOSTS.test(u.hostname) && Buffer.from(s.keys.p256dh, 'base64url').length === 65 && Buffer.from(s.keys.auth, 'base64url').length === 16;
    } catch { return false; }
  }
  function jwt(aud) {
    const head = b64u(JSON.stringify({ typ: 'JWT', alg: 'ES256' }));
    const body = b64u(JSON.stringify({ aud, exp: Math.floor(Date.now() / 1000) + 12 * 3600, sub: subject }));
    const sig = sign('sha256', Buffer.from(`${head}.${body}`), { key: priv, dsaEncoding: 'ieee-p1363' });
    return `${head}.${body}.${b64u(sig)}`;
  }
  // RFC 8291: one aes128gcm record
  function encrypt(sub, payload) {
    const ua = Buffer.from(sub.keys.p256dh, 'base64url'), auth = Buffer.from(sub.keys.auth, 'base64url');
    const ecdh = createECDH('prime256v1'); ecdh.generateKeys();
    const as = ecdh.getPublicKey(), shared = ecdh.computeSecret(ua), salt = randomBytes(16);
    const ikm = Buffer.from(hkdfSync('sha256', shared, auth, Buffer.concat([Buffer.from('WebPush: info\0'), ua, as]), 32));
    const cek = Buffer.from(hkdfSync('sha256', ikm, salt, Buffer.from('Content-Encoding: aes128gcm\0'), 16));
    const nonce = Buffer.from(hkdfSync('sha256', ikm, salt, Buffer.from('Content-Encoding: nonce\0'), 12));
    const c = createCipheriv('aes-128-gcm', cek, nonce);
    const body = Buffer.concat([c.update(Buffer.concat([Buffer.from(payload), Buffer.from([2])])), c.final(), c.getAuthTag()]);
    const head = Buffer.alloc(21); salt.copy(head); head.writeUInt32BE(4096, 16); head[20] = 65;
    return Buffer.concat([head, as, body]);
  }
  async function sendOne(sub, msg) {
    const u = new URL(sub.endpoint);
    const r = await fetch(sub.endpoint, {
      method: 'POST', body: encrypt(sub, JSON.stringify(msg)),
      headers: { TTL: '3600', Urgency: 'normal', 'Content-Encoding': 'aes128gcm', 'Content-Type': 'application/octet-stream',
                 Authorization: `vapid t=${jwt(u.origin)}, k=${vapid.publicKey}` },
    });
    return r.status;
  }
  async function notify(msg) {
    if (!subs.length) return;
    const gone = new Set();
    await Promise.all(subs.map(async (s) => {
      try { const st = await sendOne(s, msg); if (st === 404 || st === 410) gone.add(s.endpoint); else if (st >= 400) console.log(`[push] ${new URL(s.endpoint).hostname}: HTTP ${st}`); }
      catch (e) { console.log('[push] send failed:', e.message); }
    }));
    if (gone.size) { subs = subs.filter((s) => !gone.has(s.endpoint)); save(); }
    console.log(`[push] "${msg.title}" -> ${subs.length} subscriber(s)`);
  }

  const readJson = (req, cb) => {
    let body = ''; req.setEncoding('utf8');
    req.on('data', (d) => { body += d; if (body.length > 4096) req.destroy(); });
    req.on('end', () => { try { cb(JSON.parse(body)); } catch { cb(null); } });
  };
  function handle(req, res, path) {
    const json = (code, o) => { res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Cross-Origin-Resource-Policy': 'same-origin' }); res.end(JSON.stringify(o)); };
    if (path === '__push/key') return json(200, { key: vapid.publicKey });
    if (req.method !== 'POST') return json(405, {});
    readJson(req, (o) => {
      if (path === '__push/subscribe') {
        if (!o || !valid(o)) return json(400, { error: 'bad subscription' });
        if (!subs.some((s) => s.endpoint === o.endpoint)) { subs.push({ endpoint: o.endpoint, keys: { p256dh: o.keys.p256dh, auth: o.keys.auth }, t: Date.now() }); save(); }
        return json(200, { ok: true, subscribers: subs.length });
      }
      if (path === '__push/unsubscribe') {
        subs = subs.filter((s) => s.endpoint !== o?.endpoint); save();
        return json(200, { ok: true });
      }
      json(404, {});
    });
  }

  // a listed room opened: tell everyone, at most once per 10 minutes
  let lastRoomPush = 0;
  function roomOpened(r) {
    if (Date.now() - lastRoomPush < 10 * 60e3) return;
    lastRoomPush = Date.now();
    notify({ title: 'Someone wants to race', body: `${r.host || 'A player'} opened "${r.name}". Join in!`, url: `/#room=${r.code}`, tag: 'room' });
  }
  return { handle, notify, roomOpened, encrypt, get count() { return subs.length; } };
}

// Daily race: every day at HH:MM server time. next() = the next start (ms since epoch);
// live while it's under an hour old.
export function dailyRace(hhmm, notify) {
  const m = /^(\d{1,2}):(\d{2})$/.exec(hhmm || '');
  if (!m) return null;
  const at = (d) => { const t = new Date(d); t.setHours(Number(m[1]), Number(m[2]), 0, 0); return t.getTime(); };
  const next = () => { const now = Date.now(), today = at(now); return now < today + 3600e3 ? today : at(now + 864e5); };
  let warned = 0, started = 0;
  setInterval(() => {
    const t = next(), now = Date.now();
    if (now >= t - 10 * 60e3 && now < t && warned !== t) { warned = t; notify({ title: 'Daily race in 10 minutes', body: 'Open the page and join the room to race.', url: '/', tag: 'daily' }); }
    if (now >= t && now < t + 5 * 60e3 && started !== t) { started = t; notify({ title: 'The daily race is on', body: 'Players are gathering now. Join in!', url: '/', tag: 'daily' }); }
  }, 30e3).unref();
  return { next, time: hhmm };
}
