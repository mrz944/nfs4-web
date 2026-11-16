// The Players section of /__stats (analytics.mjs calls it): how many players there are and come
// back, the most active ones, and a lookup (?player=<name or id>) that puts one player's names,
// sessions (with their client logs), laps, online play and connection troubles together.
// tools/players.mjs prints the same lookup in the terminal (playerReport).
import { closeSync, openSync, readdirSync, readFileSync, readSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { browserOf } from './analytics.mjs';

// the game's track numbers (runtime/game.c), as in main.js
export const TRACKS = ['Celtic Ruins', 'Landstrasse', 'Dolphin Cove', 'Kindiak Park', 'Route Adonf', 'Durham Road', 'Snowy Ridge', 'Raceway', 'Raceway 2', 'Raceway 3',
  'Hometown', 'Redrock Ridge', 'Atlantica', 'Rocky Pass', 'Country Woods', 'Lost Canyons', 'Aquatica', 'Summit', 'Empire City'];
const DAY = 864e5;
const jsonl = (dir, prefix) => {
  const out = [];
  let files = [];
  try { files = readdirSync(dir).filter((f) => f.startsWith(prefix) && f.endsWith('.jsonl')).sort(); } catch {}
  for (const f of files) for (const line of readFileSync(join(dir, f), 'utf8').split('\n')) { if (!line) continue; try { out.push(JSON.parse(line)); } catch {} }
  return out;
};
const lapTime = (t) => `${Math.floor(t / 3840)}:${String(Math.floor(t / 64) % 60).padStart(2, '0')}.${String(Math.floor((t % 64) * 100 / 64)).padStart(2, '0')}`;
const when = (t) => (t ? new Date(t).toISOString().replace('T', ' ').slice(0, 16) : '–');
const mins = (s) => { const m = Math.round(s / 60); return m >= 60 ? `${Math.floor(m / 60)} h ${m % 60} min` : `${m} min`; };
// the start of a client log (its first line names the browser) and how many error lines it has
function peekLog(file) {
  let size = 0; try { size = statSync(file).size; } catch { return null; }
  const n = Math.min(size, 4 << 20), buf = Buffer.alloc(n), fd = openSync(file, 'r');
  try { readSync(fd, buf, 0, n, 0); } finally { closeSync(fd); }
  const text = buf.toString('utf8');
  const first = text.slice(0, text.indexOf('\n') >>> 0);
  const ua = first.replace(/^session \S+ /, '').replace(/ cores=.*$/, '');
  return { size, ua, errors: (text.match(/ERROR|ABORT/g) || []).length, partial: size > n };
}

// everything known about the players, for the section and for tools/players.mjs
export function playerData({ players, laps, playDir, sessionsDir }, events = []) {
  const sessions = jsonl(sessionsDir, 'sessions-'), play = jsonl(playDir, 'play-'), net = jsonl(playDir, 'net-');
  const by = new Map();                       // player id -> activity
  const get = (p) => { p = players.resolve(p); if (!p) return null; if (!by.has(p)) by.set(p, { sessions: [], play: 0, playN: 0, net: [], events: [], last: 0 }); return by.get(p); };
  const seen = (a, t) => { if (a && t > a.last) a.last = t; };
  for (const r of sessions) { const a = get(r.p); if (a) { a.sessions.push(r); seen(a, r.t); } }
  for (const r of play) { const a = r.p && get(r.p); if (a) { a.play += r.s; a.playN++; seen(a, r.t); } }
  for (const r of net) { const a = r.p && get(r.p); if (a) a.net.push(r); }
  for (const r of events) { const a = r.p && get(r.p); if (a) { a.events.push(r); seen(a, r.t); } }
  for (const pl of players.all()) { const a = get(pl.p); seen(a, pl.t); for (const r of laps.lapsOf(pl.p)) seen(a, r.t); }
  return { by, get: (p) => by.get(players.resolve(p)) };
}

export function playerReport(ctx, pl, events = []) {
  const d = playerData(ctx, events).get(pl.p) || { sessions: [], play: 0, playN: 0, net: [], events: [], last: pl.t };
  const own = ctx.laps.lapsOf(pl.p);
  const best = new Map();
  for (const r of own) { const k = `${r.track}:${r.f & 1 ? 'b' : 'f'}`; if (!best.has(k) || r.ticks < best.get(k).ticks) best.set(k, r); }
  const sessions = [...new Map(d.sessions.map((s) => [s.sid, s])).values()].sort((a, b) => b.t - a.t);
  return {
    pl, last: d.last, play: d.play, playN: d.playN, laps: own.length,
    best: [...best].sort((a, b) => a[0].localeCompare(b[0], undefined, { numeric: true }))
      .map(([k, r]) => ({ track: TRACKS[r.track] || `Track ${r.track + 1}`, dir: k.endsWith('b') ? 'backward' : 'forward', time: lapTime(r.ticks), car: r.car, t: r.t })),
    sessions: sessions.map((s) => ({ ...s, log: join(ctx.logDir, s.sid + '.log'), info: null })),
    net: d.net.sort((a, b) => b.t - a.t),
    events: d.events,
  };
}

export function playersSection(ctx) {
  const { players } = ctx;
  return (events, url) => {
    const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
    const card = (label, value) => `<div class="c"><div class="v">${esc(value)}</div><div class="l">${esc(label)}</div></div>`;
    const table = (head, rows) => `<table><tr>${head.map((h) => `<th>${esc(h)}</th>`).join('')}</tr>${rows.map((r) => `<tr>${r.map((c) => `<td>${esc(c)}</td>`).join('')}</tr>`).join('')}</table>`;
    const now = Date.now(), all = players.all(), data = playerData(ctx, events);
    const active = (ms) => [...data.by.values()].filter((a) => now - a.last < ms).length;
    const returning = all.filter((pl) => now - pl.t > 7 * DAY && now - (data.get(pl.p)?.last || 0) < 7 * DAY).length;
    const top = all.map((pl) => ({ pl, a: data.get(pl.p) })).filter((x) => x.a && now - x.a.last < 30 * DAY)
      .map((x) => ({ ...x, s30: x.a.sessions.filter((s) => now - s.t < 30 * DAY).length, laps: ctx.laps.lapsOf(x.pl.p).filter((r) => now - r.t < 30 * DAY).length }))
      .sort((a, b) => b.a.play - a.a.play || b.s30 - a.s30 || b.laps - a.laps).slice(0, 25);
    const q = url.searchParams.get('player') || '';
    let detail = '';
    if (q) {
      const pl = players.find(q);
      if (!pl) detail = `<p class="m">No player called ${esc(q)}.</p>`;
      else {
        const r = playerReport(ctx, pl, events);
        const sess = r.sessions.slice(0, 40).map((s) => {
          const i = peekLog(s.log), b = i ? browserOf(i.ua) : null;
          return [when(s.t), s.sid, b ? `${b.browser} ${b.version || ''} · ${b.os}` : '(no log)', i ? `${Math.round(i.size / 1024)} KB` : '', i ? i.errors + (i.partial ? '+' : '') : '', `build/clientlogs/${s.sid}.log`];
        });
        const errs = r.events.filter((e) => e.e === 'error').slice(-10).reverse();
        detail = `<h2>${esc(pl.n || '(no name)')} · ${esc(pl.p)}</h2><div class="cards">${[
          card('player since', when(pl.t)), card('last seen', when(r.last)), card('sessions', r.sessions.length), card('laps', r.laps),
          card('online play', mins(r.play)), card('online games', r.playN), card('connection events', r.net.length),
        ].join('')}</div>
<div class="cols"><div><h2>Names</h2>${table(['name', 'since'], [...pl.hist].reverse().map((h) => [h.n, when(h.t)]))}
${pl.merged.length ? `<p class="m">Joined from other keys: ${pl.merged.map((m) => esc(`${m.n || '?'} (${m.p}, ${when(m.t)})`)).join(', ')}</p>` : ''}
${pl.legacy.size ? `<p class="m">Took the laps from before player keys under: ${esc([...pl.legacy].join(', '))}</p>` : ''}</div>
<div><h2>Best laps</h2>${r.best.length ? table(['track', 'time', 'car', 'when'], r.best.map((b) => [`${b.track} ${b.dir}`, b.time, b.car, when(b.t)])) : '<p class="m">No laps.</p>'}</div></div>
<h2>Sessions</h2><div class="wrap">${sess.length ? table(['started', 'session', 'browser', 'log', 'errors', 'file'], sess) : '<p class="m">No sessions recorded.</p>'}</div>
<h2>Online connections</h2><div class="wrap">${r.net.length ? table(['when', 'event', 'room', 'detail', 'state'], r.net.slice(0, 30).map((e) => [when(e.t), e.ev, e.room || '', e.why || (e.away != null ? `back after ${e.away} ms` : ''), e.state || ''])) : '<p class="m">None.</p>'}</div>
${errs.length ? `<h2>Errors</h2><div class="wrap">${table(['when', 'kind', 'detail', 'session'], errs.map((e) => [when(e.t), e.d.kind || '', e.d.msg || '', e.s || '']))}</div>` : ''}`;
      }
    }
    return `<h2>Players</h2><div class="m">Every browser races as a player with a random id (players.mjs). Active: seen in a session, a lap or an online game.</div>
<div class="cards">${[
      card('players', all.length), card('new in 7 days', all.filter((pl) => now - pl.t < 7 * DAY).length),
      card('active today', active(DAY)), card('active in 7 days', active(7 * DAY)), card('active in 30 days', active(30 * DAY)),
      card('returning this week', returning),
    ].join('')}</div>
<form class="q" method="get"><input name="player" placeholder="name or player id" value="${esc(q)}"><button>Look up</button></form>
${detail}
<h2>Most active players</h2><div class="m">Last 30 days, by time in online games, then sessions and laps.</div>
<div class="wrap">${top.length ? table(['player', 'id', 'online play', 'sessions', 'laps', 'last seen'], top.map((x) => [x.pl.n || '(no name)', x.pl.p, mins(x.a.play), x.s30, x.laps, when(x.a.last)])) : '<p class="m">Nobody yet.</p>'}</div>`;
  };
}
