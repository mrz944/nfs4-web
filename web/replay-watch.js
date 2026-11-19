// Replays for the lap records (replays.mjs on the server), page side.
//
// Each lap the page sends carries its race's id: a new one at every race's first lap. The game
// writes savedata/replay.rp4 (its replay of the last race, the one its Replay button plays) at the
// end of every race that isn't online. When the file changes after a race whose laps went to the
// lap records, it goes to the server too, linked to those laps. The page can't see the game's
// memory, so it looks at the file every few seconds. (From NFS3 Web, where it is replay.rp3.)

const ABC = 'abcdefghjkmnpqrstuvwxyz23456789';
export function raceTracker() {
  let race = null;
  return {
    // the race a lap belongs to
    forLap(track, flags, lap, laps) {
      const f = flags & 15;
      if (!race || lap === 1 || race.track !== track || race.f !== f || race.laps !== laps || lap <= race.lap) {
        const b = crypto.getRandomValues(new Uint8Array(8));
        race = { id: [...b].map((x) => ABC[x % ABC.length]).join('') + Date.now().toString(36).slice(-4), track, f, laps, lap: 0, sent: 0, replay: false };
      }
      race.lap = lap;
      return race;
    },
    get current() { return race; },
  };
}

// the game's folder in OPFS keeps the case of the player's files: savedata/replay.rp4 in any case
async function ci(dir, name, kind) { for await (const [n, h] of dir.entries()) if (h.kind === kind && n.toLowerCase() === name) return h; return null; }
async function readReplay(dir) {
  try { const save = await ci(dir, 'savedata', 'directory'), f = save && await ci(save, 'replay.rp4', 'file'); return f ? await f.getFile() : null; } catch { return null; }
}
// a replay of the game's: its settings block (0x18a4 bytes, its first word) and then the recording
const isReplay = async (f) => f && f.size > 0x18a4 && f.size <= 8 << 20 && new DataView(await f.slice(0, 4).arrayBuffer()).getUint32(0, true) === 0x18a4;
const digest = async (file) => [...new Uint8Array(await crypto.subtle.digest('SHA-256', await file.arrayBuffer()))].slice(0, 8).map((x) => x.toString(16).padStart(2, '0')).join('');
// the replays this browser has offered the server already (their digests), newest last
const SENT = 'nfs4.replaysSent';
const sent = () => { try { return JSON.parse(localStorage.getItem(SENT) || '[]'); } catch { return []; } };
const markSent = (h) => { try { localStorage.setItem(SENT, JSON.stringify([...sent().filter((x) => x !== h), h].slice(-20))); } catch {} };
// how many times this replay has been offered late (counting this one)
function tries(h) {
  try { const o = JSON.parse(localStorage.getItem(SENT + '.tries') || '{}'); const n = o[h] = (o[h] || 0) + 1; localStorage.setItem(SENT + '.tries', JSON.stringify({ [h]: n })); return n; } catch { return 99; }
}

// When the page opens: the replay of the last race, if it never went to the server (its laps were
// all refused, or the page was closed first). The server finds its race by the file's time
// (replays.mjs lateRace). Offered once: kept or not, it isn't offered again.
// lapAt(): when the server last took a lap from this browser. The server only takes a replay
// whose race had laps on the lap records, ending 10 minutes after the file's time at most or 15
// before it, so without such a lap the replay isn't offered (it was asked on most page loads).
export async function sendLateReplay({ dir, key, skip = () => false, lapAt = () => Infinity, log = () => {} }) {
  try {
    const f = await readReplay(dir);
    if (!(await isReplay(f)) || skip() || !key()) return;
    const gap = f.lastModified - lapAt();
    if (!(gap > -10 * 60e3 && gap < 15 * 60e3)) return;
    const h = await digest(f);
    if (sent().includes(h)) return;
    const res = await fetch(`__replay?race=late&at=${Math.round(f.lastModified)}`, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream', 'x-player-key': key() }, body: await f.arrayBuffer() });
    const d = await res.json().catch(() => ({}));
    // kept, or a race it can't be: done. No race for it (yet): asked again on the next 4 visits.
    if (res.ok || res.status === 409 || (res.status === 404 && tries(h) >= 5)) markSent(h);
    log(res.ok ? `replay: the last race's replay sent late, linked to the laps of race ${d.race}` : `replay: the last race's replay not taken (${d.error || res.status})`);
  } catch (e) { log('replay: late one not sent:', e.message); }
}

// dir: the game's folder (OPFS); race(): the current race; key(): the player key (token);
// skip(): true while mods are on
export async function watchReplay({ dir, race, key, skip = () => false, log = () => {}, every = 4000 }) {
  const read = () => readReplay(dir);
  const f = await read();
  let stamp = f ? f.lastModified : 0, seen = f ? await digest(f).catch(() => '') : '', busy = false;
  async function look() {
    if (busy) return;
    busy = true;
    try {
      const g = await read();
      if (!g || g.lastModified === stamp) return;
      stamp = g.lastModified;
      const h = await digest(g).catch(() => null);
      if (!h || h === seen) return;
      seen = h;
      const r = race();
      log(`replay: the game wrote replay.rp4 (${g.size} bytes)${r ? `, race ${r.id}: ${r.sent} lap(s) sent` : ''}${skip() ? ', mods on' : ''}`);
      if (!r || !r.sent || r.replay || skip() || !key() || !(await isReplay(g))) return;
      r.replay = true;
      const res = await fetch(`__replay?race=${r.id}`, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream', 'x-player-key': key() }, body: await g.arrayBuffer() });
      const d = await res.json().catch(() => ({}));
      if (res.status < 500 && res.status !== 429) markSent(h);
      log(res.ok ? `replay: sent, linked to the laps of race ${r.id}` : `replay: not taken (${d.error || res.status})`);
    } catch (e) { log('replay: not sent:', e.message); } finally { busy = false; }
  }
  const timer = setInterval(look, every);
  return { look, stop: () => clearInterval(timer) };
}
