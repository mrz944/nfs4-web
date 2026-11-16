// Replays for the lap records: the game's own replay of the race a best lap was driven in.
//
// At the end of every race that isn't online, NFS4 writes savedata/replay.rp4 ("%sreplay.rp4"):
// a copy of the game's settings block (0x18a4 bytes, which its first word says; the game's own
// at 0x7c3c04: track, laps, direction, mirrored, night, weather, the racers and their cars and
// names), then the recording, so its size depends on the race (~170 KB for 2 laps of Celtic
// Ruins).
// The page (main.js watchReplay) sees the file change after a race whose laps it sent, and
// uploads it here with the player key: POST /__replay?race=<id>. It is taken only when that
// race's laps are this player's and the file's settings are theirs, and kept gzipped in
// build/replays/<race>.rp4.gz. GET /__replay/<race>.rp4 gives it back; /__laps links it on
// that race's laps (laps.mjs). The game plays saved replays from savedata/Saves/<name>.rp4
// (Load > Replays), which is where the start page puts it.
// A replay that didn't go up when its race ended (its laps were refused, or the page closed)
// comes later, when the page opens: POST /__replay?race=late&at=<the file's time> (lateRace).
// Replays of races with no best lap on any board are deleted after an hour.
// (From NFS3 Web's replays.mjs: there the file is replay.rp3, of one fixed size.)
import { gunzipSync, gzipSync } from 'node:zlib';
import { mkdirSync, readdirSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { clientIp, foreignOrigin } from './relay.mjs';
import { TOKEN } from './players.mjs';

export const CONFIG_SIZE = 0x18a4, MAX_SIZE = 8 << 20;
// race settings in the settings copy at the start of the file (runtime/game.c RACE_*: 0x7c4ce4..
// - 0x7c3c04)
const AT = { track: 0x10e0, laps: 0x10e8, backward: 0x10ec, mirrored: 0x10f0, night: 0x10f4, weather: 0x10f8 };
const FLAG = { backward: 1, mirrored: 2, night: 4, weather: 8 };
const PER_HOUR = 30;
const ID = /^[a-z0-9]{8,16}$/;

// the race a replay is of, from its config copy (null when it isn't one of the game's replays)
export function replayInfo(buf) {
  if (buf.length <= CONFIG_SIZE || buf.length > MAX_SIZE || buf.readUInt32LE(0) !== CONFIG_SIZE) return null;
  const u = (k) => buf.readUInt32LE(AT[k]);
  let f = 0;
  for (const k of Object.keys(FLAG)) if (u(k)) f |= FLAG[k];
  return { track: u('track'), flags: f, laps: u('laps') };
}

export function createReplays(dir, { players, laps }) {
  mkdirSync(dir, { recursive: true });
  const have = new Set(readdirSync(dir).filter((f) => /^[a-z0-9]+\.rp4\.gz$/.test(f)).map((f) => f.split('.')[0]));
  laps.hasReplay = (id) => have.has(id);
  const file = (id) => join(dir, `${id}.rp4.gz`);

  const recent = new Map();             // ip -> [uploads, hour]
  function send(res, code, o) {
    res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(o));
  }
  function handlePost(req, res, url) {
    if (foreignOrigin(req)) { res.writeHead(403).end(); req.resume(); return; }
    const ip = clientIp(req), hour = Math.floor(Date.now() / 3600e3), c = recent.get(ip);
    if (c && c[1] === hour && c[0] >= PER_HOUR) { req.resume(); send(res, 429, { error: 'busy' }); return; }
    if (recent.size > 10000) recent.clear();
    recent.set(ip, [c && c[1] === hour ? c[0] + 1 : 1, hour]);
    const at = Number(url.searchParams.get('at')), key = String(req.headers['x-player-key'] || '');
    let race = url.searchParams.get('race') || '';
    const chunks = []; let size = 0;
    req.on('data', (d) => { size += d.length; if (size > MAX_SIZE) req.destroy(); else chunks.push(d); });
    req.on('end', () => {
      const late = race === 'late';
      if ((!late && !ID.test(race)) || (late && !(at > 0)) || !TOKEN.test(key)) { send(res, 400, { error: 'bad' }); return; }
      const pl = players.byToken(key);
      if (!pl) { send(res, 404, { error: 'unknown' }); return; }
      const buf = Buffer.concat(chunks), info = replayInfo(buf);
      if (!info) { send(res, 400, { error: 'not a replay' }); return; }
      const fits = (r) => info.track === r.track && info.flags === (r.f & 15) && info.laps === r.laps;
      if (late) race = lateRace(pl.p, info, at, fits);
      if (!race) { send(res, 404, { error: 'no such race' }); return; }
      const own = laps.lapsOfRace(race);
      if (!own.length || own.some((r) => laps.owner(r) !== pl.p)) { send(res, 404, { error: 'no such race' }); return; }
      const r = own[0];
      if (!fits(r)) { send(res, 409, { error: 'another race', info }); return; }
      if (have.has(race)) { send(res, 200, { ok: true, had: true }); return; }
      try {
        writeFileSync(file(race) + '.tmp', gzipSync(buf, { level: 9 }));
        renameSync(file(race) + '.tmp', file(race));
      } catch (e) { console.log('[replays] write failed:', e.message); send(res, 500, { error: 'write' }); return; }
      have.add(race); laps.changed();
      console.log(`[replays] ${pl.n} (${pl.p}): race ${race}, track ${r.track}, ${own.length} lap(s)${late ? ', sent late' : ''}`);
      send(res, 200, { ok: true, race });
    });
  }
  // A replay the page didn't send when its race ended (POST /__replay?race=late&at=<the file's
  // time>): replay.rp4 stays in the browser until the next race, and the page offers it when it
  // opens. Its race: this player's race without a replay, with the file's settings, whose last
  // lap came closest to the file's time (15 minutes before it at most; laps put back from the
  // page logs carry times up to a few minutes late, and the clocks differ a little).
  function lateRace(p, info, at, fits) {
    let best = null, gap = Infinity;
    const end = new Map();                 // race -> its last lap's time
    for (const r of laps.lapsOf(p)) if (r.race && !have.has(r.race) && fits(r)) end.set(r.race, Math.max(end.get(r.race) || 0, r.t));
    for (const [id, t] of end) {
      const d = at - t;
      if (d < -10 * 60e3 || d > 15 * 60e3) continue;
      if (Math.abs(d) < gap) { gap = Math.abs(d); best = id; }
    }
    return best;
  }
  function handleGet(req, res, url) {
    const id = (/^\/__replay\/([a-z0-9]+)\.rp4$/.exec(url.pathname) || [])[1];
    if (!id || !have.has(id)) { res.writeHead(404).end('not found'); return; }
    let body;
    try { body = gunzipSync(readFileSync(file(id))); } catch { res.writeHead(404).end('not found'); return; }
    res.writeHead(200, {
      'Content-Type': 'application/octet-stream', 'Content-Length': body.length, 'Cache-Control': 'public, max-age=86400',
      'Content-Disposition': `attachment; filename="nfs4-${id}.rp4"`, 'Cross-Origin-Resource-Policy': 'same-origin',
    });
    res.end(req.method === 'HEAD' ? undefined : body);
  }
  // replays no board needs: gone an hour after they came (a race's laps all arrive before it)
  function prune() {
    const keep = laps.bestRaces();
    for (const id of [...have]) {
      if (keep.has(id)) continue;
      try { if (Date.now() - statSync(file(id)).mtimeMs < 3600e3) continue; unlinkSync(file(id)); } catch {}
      have.delete(id);
    }
    laps.changed();
  }
  setInterval(prune, 3600e3).unref();
  return { handlePost, handleGet, prune, has: (id) => have.has(id) };
}
