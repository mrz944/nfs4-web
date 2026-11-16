// Lap records for the start page: the best lap per player on every track, each direction.
//
// The game page reports each lap the player finishes (POST /__lap, see reportLap() in main.js;
// the runtime reads the lap off the game, runtime/game.c). Every lap is appended to
// build/laps/laps-YYYY-MM.jsonl; GET /__laps gives the boards: per track and direction, each
// player's best lap, fastest first, under the player's current name. Times are in the game's
// own 1/64 s ticks. Tracks are the game's track numbers (0 Celtic Ruins .. 18 Empire City, the
// TRACKS list in main.js). Same as NFS3 Web's laps.mjs, which this file comes from.
//
// Players (players.mjs): a lap comes with the page's player key, and is stored with the
// player's public id (p) and the name it had then. Laps from before player keys have only a
// name: they count for the player who took that name (players.mjs 'legacy'), else they stay
// one row per (folded) name until someone does. A lap without a key (a page from before keys,
// still open) is taken only for a name no player has.
// Replays (replays.mjs): each lap carries its race's id (race, made by the page); when the
// game's replay of that race comes in, the boards link it on that race's laps (rp, lap, laps).
// Laps driven with mods on never get here (the page doesn't send them). The page sends them,
// so a determined cheat can post anything: the checks below only keep out nonsense.
import { appendFileSync, mkdirSync, readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { badName, cleanName, clientIp, foreignOrigin } from './relay.mjs';
import { foldName, TOKEN } from './players.mjs';

export const FLAG = { backward: 1, mirrored: 2, night: 4, weather: 8, instant: 16 };   // (NFS4 has no instant steering: 16 is NFS3's)
const TOP = 20;
const MIN_TICKS = 64 * 20, MAX_TICKS = 64 * 60 * 20;   // 20 s .. 20 min a lap

export function createLaps(dir, players) {
  mkdirSync(dir, { recursive: true });
  const laps = [];                    // every lap, oldest first
  const legacy = new Map();           // folded name -> laps from before player keys under it
  const races = new Map();            // race id -> its laps
  const api = { hasReplay: () => false };   // replays.mjs: is there a replay of this race?
  let cache = null, cacheFor = '', json = null;

  function keep(r) {
    laps.push(r);
    if (r.race) { if (!races.has(r.race)) races.set(r.race, []); races.get(r.race).push(r); }
    if (!r.p) { const f = foldName(r.n); legacy.set(f, (legacy.get(f) || 0) + 1); }
  }
  for (const f of readdirSync(dir).filter((f) => /^laps-.*\.jsonl$/.test(f)).sort()) {
    for (const line of readFileSync(join(dir, f), 'utf8').split('\n')) {
      if (!line) continue;
      try { keep(JSON.parse(line)); } catch {}
    }
  }
  players.legacyLaps = (f) => legacy.has(f);

  // whose a lap is now: a player's id, or "n:<folded name>" for old laps nobody has taken
  // (null: its player was deleted)
  function owner(r) {
    if (r.p) return players.resolve(r.p);
    const f = foldName(r.n), lo = players.legacyOwner(f);
    return lo === undefined ? 'n:' + f : lo;
  }
  const hasRp = (r) => !!r.race && api.hasReplay(r.race);
  // "track:f|b" -> Map(owner -> best lap)
  function boards() {
    const stamp = `${laps.length}:${players.version}`;
    if (cache && cacheFor === stamp) return cache;
    const b = new Map();
    for (const r of laps) {
      const o = owner(r); if (!o) continue;
      const key = `${r.track}:${r.f & FLAG.backward ? 'b' : 'f'}`;
      if (!b.has(key)) b.set(key, new Map());
      const m = b.get(key), had = m.get(o);
      // the same time again: the lap with a replay
      if (!had || r.ticks < had.r.ticks || (r.ticks === had.r.ticks && hasRp(r) && !hasRp(had.r))) m.set(o, { o, r });
    }
    cacheFor = stamp; cache = b; json = null;
    return b;
  }
  function boardsJson() {
    const b = boards();
    if (json) return json;
    const out = {};
    for (const [key, m] of b) out[key] = [...m.values()].sort((x, y) => x.r.ticks - y.r.ticks || x.r.t - y.r.t).slice(0, TOP)
      .map(({ o, r }) => ({ ...(o.startsWith('n:') ? {} : { p: o }), n: o.startsWith('n:') ? r.n : players.nameOf(o) || r.n, car: r.car, ticks: r.ticks, f: r.f, t: r.t,
        ...(r.race && api.hasReplay(r.race) ? { rp: r.race, lap: r.lap, ...(r.laps >= r.lap ? { laps: r.laps } : {}) } : {}) }));
    json = JSON.stringify({ boards: out });
    return json;
  }

  const recent = new Map();           // ip -> [count, 10-minute window]
  function handlePost(req, res) {
    if (foreignOrigin(req)) { res.writeHead(403).end(); req.resume(); return; }
    let body = '';
    req.setEncoding('utf8');
    req.on('data', (d) => { body += d; if (body.length > 1024) req.destroy(); });
    req.on('end', () => {
      const ip = clientIp(req), win = Math.floor(Date.now() / 600e3), c = recent.get(ip);
      if (c && c[1] === win && c[0] >= 40) { res.writeHead(429).end(); return; }
      if (recent.size > 10000) recent.clear();
      recent.set(ip, [c && c[1] === win ? c[0] + 1 : 1, win]);
      let m; try { m = JSON.parse(body); } catch { res.writeHead(400).end(); return; }
      const int = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
      let n = cleanName(m?.n);
      // laps: the race's laps, as the game's settings had them (a lap past it can still be real:
      // a knockout round, say)
      if (!n || badName(n) || !int(m.track, 0, 63) || !int(m.f, 0, 31) || !int(m.lap, 1, 16) || !int(m.laps, 0, 99)
          || !int(m.ticks, MIN_TICKS, MAX_TICKS) || typeof m.car !== 'string' || !/^[a-z0-9]{1,4}$/.test(m.car)) { res.writeHead(400).end(); return; }
      const send = (code, o) => { res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }); res.end(JSON.stringify(o)); };
      let p = null;
      if (TOKEN.test(m.k || '')) {
        const u = players.use(m.k, n, ip);
        if (u.error) { send(u.error === 'busy' ? 429 : 409, { error: u.error }); return; }
        p = u.pl.p; n = u.pl.n;
        players.session(u.pl, m.s);
      } else if (players.nameState(n).state === 'taken') { send(409, { error: 'taken' }); return; }
      const race = typeof m.race === 'string' && /^[a-z0-9]{8,16}$/.test(m.race) ? m.race : null;
      const r = { t: Date.now(), n, ...(p ? { p } : {}), ...(race ? { race } : {}), track: m.track, f: m.f, car: m.car, ticks: m.ticks, lap: m.lap, laps: m.laps, b: String(m.b || '').slice(0, 24) };
      const o = owner(r), had = o && boards().get(`${r.track}:${r.f & FLAG.backward ? 'b' : 'f'}`)?.get(o);
      try { appendFileSync(join(dir, `laps-${new Date().toISOString().slice(0, 7)}.jsonl`), JSON.stringify(r) + '\n'); } catch (e) { console.log('[laps] write failed:', e.message); }
      keep(r);
      send(200, { best: !had || r.ticks < had.r.ticks, ...(p ? { p, n } : {}) });
    });
  }

  function handleGet(req, res) {
    const body = boardsJson();
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Cross-Origin-Resource-Policy': 'same-origin' });
    res.end(body);
  }
  // for the stats page and tools/players.mjs: a player's laps (their own and the old ones they took)
  const lapsOf = (p) => laps.filter((r) => owner(r) === p);
  // replays.mjs: a race's laps, and the races whose laps are someone's best on a board (their
  // replays are kept, the others go)
  const lapsOfRace = (id) => races.get(id) || [];
  function bestRaces() {
    const out = new Set();
    for (const m of boards().values()) for (const { r } of m.values()) if (r.race) out.add(r.race);
    return out;
  }
  const changed = () => { json = null; cache = null; };     // a replay came or went
  return Object.assign(api, { handlePost, handleGet, lapsOf, owner, lapsOfRace, bestRaces, changed });
}
