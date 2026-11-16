// Players: a name, its lap records and its logs, tied to a passphrase instead of an account.
//
// Every browser makes its own player key: 8 words from the EFF short word list (words.js),
// kept in its localStorage (main.js). The page sends SHA-256("nfs4 player v1:" + words), the
// token, and never the words; this file keeps only SHA-256(token) and a random public id (p,
// 8 characters) that the boards and logs use. The same words on another device are the same
// player there: that is the whole "log-in".
//
// A player is made on first use: a finished lap, joining a room, a name change, or opening the
// passphrase. Also on the first visit when the browser's saved name has lap records from before
// player keys that nobody has taken yet: that is how the old boards move over (laps.mjs keeps
// those laps under their names until someone takes them).
//
// A name belongs to one player. Names are compared folded (case, separators and look-alike
// digits ignored: DESTOR = destor = D3ST0R). After a change the old name stays its player's for
// HOLD_MS, then it is free. NAME_CHANGES a day.
//
// build/players/players.jsonl, append-only; read at start, and again when another process
// (tools/players.mjs) appends to it:
//   {t, ev:'new', p, h}          a player (h: SHA-256 of the token, hex)
//   {t, ev:'name', p, n}         took or changed its name (admin: by tools/players.mjs)
//   {t, ev:'legacy', p, n}       took the laps from before player keys under name n
//   {t, ev:'merge', p, from}     player `from` joined p: its laps and key are p's now
//   {t, ev:'forget', p}          deleted: its laps leave the boards, its names are free
// build/players/sessions-YYYY-MM.jsonl: {t, sid, p}, which page sessions (client logs) were whose.
import { createHash, randomBytes } from 'node:crypto';
import { appendFileSync, mkdirSync, readFileSync, unwatchFile, watchFile } from 'node:fs';
import { join } from 'node:path';
import { badName, cleanName, clientIp, foreignOrigin } from './relay.mjs';

export const HOLD_MS = 7 * 864e5, NAME_CHANGES = 3;
export const TOKEN = /^[0-9a-f]{64}$/;
const NEW_PER_HOUR = 10, UNKNOWN_PER_HOUR = 60;
const FOLD = { 0: 'o', 1: 'i', l: 'i', '|': 'i', 3: 'e', 4: 'a', 5: 's', 7: 't' };
export const foldName = (n) => String(n ?? '').toLowerCase().replace(/[\s._!-]/g, '').replace(/[01l|3457]/g, (c) => FOLD[c]);
const hash = (s) => createHash('sha256').update(s).digest('hex');
const ID_ABC = 'abcdefghjkmnpqrstuvwxyz23456789';

export function createPlayers(dir, { now = Date.now, watch = true } = {}) {
  mkdirSync(dir, { recursive: true });
  const file = join(dir, 'players.jsonl');
  let S, knownSize = 0, version = 0;
  const api = {
    legacyLaps: () => false,              // laps.mjs: are there laps from before keys under this folded name?
  };

  function fresh() {
    return { byHash: new Map(), byId: new Map(), alias: new Map(), names: new Map(), legacy: new Map() };
  }
  // follows merges; null for a player that was deleted
  function resolve(p) {
    for (let i = 0; p && i < 64; i++) { if (S.byId.has(p)) return p; p = S.alias.get(p); }
    return null;
  }
  function apply(e) {
    const pl = S.byId.get(e.p);
    if (e.ev === 'new') {
      if (S.byId.has(e.p) || S.alias.has(e.p)) return;
      const n = { p: e.p, hashes: new Set([e.h]), n: '', hist: [], changes: [], t: e.t, legacy: new Set(), merged: [] };
      S.byId.set(e.p, n); S.byHash.set(e.h, e.p);
    } else if (e.ev === 'name' && pl) {
      const old = pl.n;
      if (old) S.names.set(foldName(old), { p: pl.p, until: e.t + HOLD_MS });
      if (old && !e.admin && foldName(old) !== foldName(e.n)) pl.changes.push(e.t);
      pl.n = e.n; pl.hist.push({ n: e.n, t: e.t });
      S.names.set(foldName(e.n), { p: pl.p, until: null });
    } else if (e.ev === 'legacy' && pl) {
      const f = foldName(e.n), was = S.legacy.get(f);
      if (was && S.byId.has(was)) S.byId.get(was).legacy.delete(f);
      pl.legacy.add(f); S.legacy.set(f, pl.p);
    } else if (e.ev === 'merge' && pl) {
      const from = S.byId.get(e.from);
      if (!from || from === pl) return;
      for (const h of from.hashes) { pl.hashes.add(h); S.byHash.set(h, pl.p); }
      for (const f of from.legacy) { pl.legacy.add(f); S.legacy.set(f, pl.p); }
      for (const [f, v] of S.names) if (v.p === from.p) S.names.delete(f);
      pl.merged.push({ p: from.p, n: from.n, t: e.t }, ...from.merged);
      S.byId.delete(from.p); S.alias.set(from.p, pl.p);
    } else if (e.ev === 'forget' && pl) {
      for (const h of pl.hashes) S.byHash.delete(h);
      for (const [f, v] of S.names) if (v.p === pl.p) S.names.delete(f);
      S.byId.delete(pl.p);            // its old names stay in S.legacy: those laps stay off the boards
    }
  }
  function load() {
    S = fresh();
    let text = '';
    try { text = readFileSync(file, 'utf8'); } catch {}
    for (const line of text.split('\n')) { if (!line) continue; try { apply(JSON.parse(line)); } catch {} }
    knownSize = Buffer.byteLength(text); version++;
  }
  function write(e) {
    const rec = { t: now(), ...e }, line = JSON.stringify(rec) + '\n';
    appendFileSync(file, line);
    knownSize += Buffer.byteLength(line);
    apply(rec); version++;
    return rec;
  }
  load();
  // tools/players.mjs appends while the server runs: read the file again when it grew
  if (watch) watchFile(file, { interval: 2000, persistent: false }, (cur) => { if (cur.size !== knownSize) load(); });

  const byToken = (token) => (TOKEN.test(token || '') ? S.byId.get(S.byHash.get(hash(token))) || null : null);
  function nameState(name, p = null) {
    const n = cleanName(name);
    if (!n || badName(n) || !foldName(n)) return { state: 'bad', n };
    const f = foldName(n), e = S.names.get(f);
    if (e && (e.until == null || e.until > now()) && resolve(e.p)) return { state: resolve(e.p) === p ? 'yours' : 'taken', n, owner: resolve(e.p) };
    const lo = S.legacy.get(f);
    if (lo && resolve(lo)) return { state: resolve(lo) === p ? 'yours' : 'taken', n, owner: resolve(lo) };
    return { state: api.legacyLaps(f) && !lo ? 'old' : 'free', n };      // (lo: a deleted player had them)
  }
  const made = new Map();                  // address -> [new players, hour]
  function create(token, ip) {
    if (!TOKEN.test(token || '')) return null;
    const have = byToken(token); if (have) return have;
    const hour = Math.floor(now() / 3600e3), c = made.get(ip);
    if (ip && c && c[1] === hour && c[0] >= NEW_PER_HOUR) return null;
    if (made.size > 10000) made.clear();
    if (ip) made.set(ip, [c && c[1] === hour ? c[0] + 1 : 1, hour]);
    let p;
    do p = Array.from(randomBytes(8), (b) => ID_ABC[b % ID_ABC.length]).join(''); while (S.byId.has(p) || S.alias.has(p));
    write({ ev: 'new', p, h: hash(token) });
    return S.byId.get(p);
  }
  // take or change a player's name; the laps from before keys under it come along if nobody has them
  function setName(pl, name, { admin = false } = {}) {
    const st = nameState(name, pl.p);
    if (st.state === 'bad') return { error: 'bad' };
    if (st.state === 'taken' && !admin) return { error: 'taken' };
    if (pl.n === st.n) return { ok: true, n: pl.n };
    const t = now();
    if (pl.n && !admin && foldName(pl.n) !== foldName(st.n) && pl.changes.filter((x) => t - x < 864e5).length >= NAME_CHANGES) return { error: 'limit' };
    write({ ev: 'name', p: pl.p, n: st.n, ...(admin ? { admin: true } : {}) });
    let claimed = false;
    if (st.state === 'old') { write({ ev: 'legacy', p: pl.p, n: st.n }); claimed = true; }
    return { ok: true, n: st.n, claimed };
  }
  // a request with a key: its player, made (and named) if needed
  function use(token, name, ip, { make = true } = {}) {
    let pl = byToken(token);
    if (!pl && make) pl = create(token, ip);
    if (!pl) return { error: TOKEN.test(token || '') && make ? 'busy' : 'unknown' };
    if (!pl.n) { const r = setName(pl, name); if (r.error) return { pl, error: r.error }; return { pl, named: r }; }
    return { pl };
  }
  const seenSessions = new Set();
  function session(pl, sid) {
    sid = String(sid || '').replace(/[^a-z0-9-]/gi, '').slice(0, 40);
    if (!pl || !sid || seenSessions.has(sid + pl.p)) return;
    if (seenSessions.size > 50000) seenSessions.clear();
    seenSessions.add(sid + pl.p);
    try { appendFileSync(join(dir, `sessions-${new Date(now()).toISOString().slice(0, 7)}.jsonl`), JSON.stringify({ t: now(), sid, p: pl.p }) + '\n'); } catch {}
  }
  const pub = (pl, extra = {}) => ({ p: pl.p, n: pl.n, names: [...new Set(pl.hist.map((h) => h.n))], t: pl.t, ...extra });

  // ---- HTTP: /__me, /__name, /__merge, /__forget (main.js)
  const unknown = new Map();               // address -> [lookups of keys nobody has, hour]
  const tooManyUnknown = (ip) => { const hour = Math.floor(now() / 3600e3), c = unknown.get(ip); return c && c[1] === hour && c[0] >= UNKNOWN_PER_HOUR; };
  const countUnknown = (ip) => { const hour = Math.floor(now() / 3600e3), c = unknown.get(ip); if (unknown.size > 10000) unknown.clear(); unknown.set(ip, [c && c[1] === hour ? c[0] + 1 : 1, hour]); };
  function reply(res, code, o) {
    res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Cross-Origin-Resource-Policy': 'same-origin' });
    res.end(JSON.stringify(o));
  }
  const ERR = { taken: 409, bad: 400, limit: 429, busy: 429, unknown: 404 };
  function handle(req, res, url) {
    if (foreignOrigin(req)) { res.writeHead(403).end(); req.resume(); return; }
    const ip = clientIp(req);
    if (req.method === 'GET' && url.pathname === '/__name') { reply(res, 200, (({ state, owner }) => ({ state, owner }))(nameState(url.searchParams.get('n')))); return; }
    if (req.method !== 'POST') { res.writeHead(405).end(); req.resume(); return; }
    let body = '';
    req.setEncoding('utf8');
    req.on('data', (d) => { body += d; if (body.length > 2048) req.destroy(); });
    req.on('end', () => {
      let m; try { m = JSON.parse(body); } catch { reply(res, 400, { error: 'bad' }); return; }
      if (!m || typeof m !== 'object' || !TOKEN.test(m.k || '')) { reply(res, 400, { error: 'bad' }); return; }
      const path = url.pathname;
      if (path === '/__me') {
        // who this key is; a key nobody has yet is made only when the browser's saved name has
        // old laps nobody has taken (the move from the boards before keys)
        let pl = byToken(m.k);
        if (!pl) {
          if (tooManyUnknown(ip)) { reply(res, 429, { error: 'busy' }); return; }
          countUnknown(ip);
          const st = m.n ? nameState(m.n) : { state: 'free' };
          if (st.state !== 'old') { reply(res, 200, { p: null, state: st.state }); return; }
          const r = use(m.k, m.n, ip);
          if (r.error) { reply(res, 200, { p: null, state: st.state }); return; }
          session(r.pl, m.s);
          reply(res, 200, pub(r.pl, { claimed: true })); return;
        }
        session(pl, m.s);
        reply(res, 200, pub(pl)); return;
      }
      if (path === '/__name') {
        const r = use(m.k, m.n, ip);
        if (r.error && !r.pl) { reply(res, ERR[r.error] || 400, { error: r.error }); return; }
        const s = r.error ? { error: r.error } : r.named || setName(r.pl, m.n);
        session(r.pl, m.s);
        if (s.error) { reply(res, ERR[s.error] || 400, { error: s.error, ...pub(r.pl) }); return; }
        reply(res, 200, pub(r.pl, { claimed: !!s.claimed })); return;
      }
      if (path === '/__merge') {
        // both keys are known here (the page has its own and was just given the other one)
        const pl = byToken(m.k), from = byToken(m.from);
        if (!pl) { countUnknown(ip); reply(res, 404, { error: 'unknown' }); return; }
        if (from && from !== pl) write({ ev: 'merge', p: pl.p, from: from.p });
        reply(res, 200, pub(pl)); return;
      }
      if (path === '/__forget') {
        const pl = byToken(m.k);
        if (pl) write({ ev: 'forget', p: pl.p });
        reply(res, 200, { ok: true }); return;
      }
      reply(res, 404, { error: 'unknown' });
    });
  }

  Object.defineProperty(api, 'version', { get: () => version });   // laps.mjs: rebuild the boards when it changes
  return Object.assign(api, {
    file, dir, handle, nameState, setName, use, create, session, resolve, byToken, pub, write,
    // the player of a public id (after merges), its current name, the owner of an old name
    get: (p) => S.byId.get(resolve(p)) || null,
    nameOf: (p) => S.byId.get(resolve(p))?.n || '',
    // undefined: nobody took them; null: a player who was deleted did
    legacyOwner: (f) => (S.legacy.has(f) ? resolve(S.legacy.get(f)) : undefined),
    idOfToken: (token) => byToken(token)?.p || null,
    all: () => [...S.byId.values()],
    // by name (current, then earlier ones) or id
    find(q) {
      q = String(q || '').trim(); if (!q) return null;
      if (S.byId.has(resolve(q))) return S.byId.get(resolve(q));
      const f = foldName(q), e = S.names.get(f);
      if (e && resolve(e.p)) return S.byId.get(resolve(e.p));
      if (S.legacy.get(f) && resolve(S.legacy.get(f))) return S.byId.get(resolve(S.legacy.get(f)));
      return [...S.byId.values()].find((pl) => pl.hist.some((h) => foldName(h.n) === f)) || null;
    },
    reload: load,
    close: () => unwatchFile(file),
  });
}
