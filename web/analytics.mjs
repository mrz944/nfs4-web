// Self-hosted page analytics. No cookies, no third parties, no raw IPs stored.
//
// The page sends small events (POST /__event, see track() in main.js). Each one is
// appended to build/analytics/events-YYYY-MM.jsonl with a visitor id that is a hash
// of (daily random salt, IP, user agent): the same visitor counts once a day, but
// visitors can't be followed across days and the IP can't be recovered.
// GET /__stats renders a summary of the last 30 days plus the live rooms.
import { createHash, randomBytes } from 'node:crypto';
import { appendFileSync, mkdirSync, readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const EVENTS = new Set(['view', 'import', 'start', 'first_frame', 'load', 'room_create', 'room_join', 'online_start', 'push', 'error', 'end']);
// load phases (main.js mark()): seconds since the page opened
const PHASES = [['download', 'game download (first visit)'], ['files', 'game files ready'], ['click', 'Start pressed'], ['wasm', 'runtime running'], ['first', 'first frame'], ['shown', 'game visible']];
let salt = { day: '', value: '' };
const today = () => new Date().toISOString().slice(0, 10);
function visitor(req) {
  if (salt.day !== today()) salt = { day: today(), value: randomBytes(16).toString('hex') };
  const ip = req.headers['x-forwarded-for']?.split(',')[0].trim() || req.socket.remoteAddress || '';
  return createHash('sha256').update(salt.value + ip + (req.headers['user-agent'] || '')).digest('hex').slice(0, 16);
}
export function browserOf(ua = '') {
  const os = /iPhone|iPad/.test(ua) ? 'iOS' : /Android/.test(ua) ? 'Android' : /Mac OS X/.test(ua) ? 'macOS' : /Windows/.test(ua) ? 'Windows' : /Linux/.test(ua) ? 'Linux' : 'other';
  const m = ua.match(/(Edg|Firefox|Chrome|Version)\/(\d+)/);
  const name = !m ? 'other' : m[1] === 'Edg' ? 'Edge' : m[1] === 'Version' ? 'Safari' : m[1];
  return { browser: name, version: m ? Number(m[2]) : 0, os };
}

export function createAnalytics(dir) {
  mkdirSync(dir, { recursive: true });
  const fileFor = (d = new Date()) => join(dir, `events-${d.toISOString().slice(0, 7)}.jsonl`);

  function handleEvent(req, res) {
    let body = '';
    req.setEncoding('utf8');
    req.on('data', (d) => { body += d; if (body.length > 4096) req.destroy(); });
    req.on('end', () => {
      res.writeHead(204).end();
      let e; try { e = JSON.parse(body); } catch { return; }
      if (!EVENTS.has(e?.e)) return;
      const data = {};
      for (const [k, v] of Object.entries(e.d || {}).slice(0, 12)) {
        if (typeof v === 'number' && Number.isFinite(v)) data[k] = Math.round(v * 100) / 100;
        else if (typeof v === 'boolean') data[k] = v;
        else if (typeof v === 'string') data[k] = v.slice(0, 80);
      }
      const rec = { t: Date.now(), e: e.e, v: visitor(req), s: String(e.s || '').slice(0, 40), d: data };
      if (e.e === 'view') Object.assign(rec.d, browserOf(req.headers['user-agent']));
      appendFileSync(fileFor(), JSON.stringify(rec) + '\n');
    });
  }

  function load(days) {
    const since = Date.now() - days * 864e5, out = [];
    for (const f of readdirSync(dir).filter((f) => f.startsWith('events-')).sort().slice(-2)) {
      for (const line of readFileSync(join(dir, f), 'utf8').split('\n')) {
        if (!line) continue;
        try { const r = JSON.parse(line); if (r.t >= since) out.push(r); } catch {}
      }
    }
    return out;
  }

  function handleStats(req, res, live) {
    const ev = load(30);
    const by = (name) => ev.filter((r) => r.e === name);
    const uniq = (arr, k) => new Set(arr.map(k)).size;
    const median = (a) => { if (!a.length) return null; const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
    const days = new Map();
    for (const r of ev) {
      const d = new Date(r.t).toISOString().slice(0, 10);
      if (!days.has(d)) days.set(d, { views: 0, visitors: new Set(), starts: 0, online: 0, errors: 0 });
      const x = days.get(d);
      if (r.e === 'view') { x.views++; x.visitors.add(r.v); }
      if (r.e === 'start') x.starts++;
      if (r.e === 'online_start') x.online++;
      if (r.e === 'error') x.errors++;
    }
    const count = (arr, key) => { const m = new Map(); for (const r of arr) { const k = key(r); m.set(k, (m.get(k) || 0) + 1); } return [...m].sort((a, b) => b[1] - a[1]); };
    const views = by('view'), starts = by('start'), ends = by('end').filter((r) => r.d.played);
    const loadS = median(by('first_frame').map((r) => r.d.secs)), playM = median(ends.map((r) => r.d.secs / 60));
    const fps = median(ends.map((r) => r.d.fps).filter((x) => x > 0));
    const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
    const card = (label, value) => `<div class="c"><div class="v">${esc(value)}</div><div class="l">${esc(label)}</div></div>`;
    const table = (head, rows) => `<table><tr>${head.map((h) => `<th>${esc(h)}</th>`).join('')}</tr>${rows.map((r) => `<tr>${r.map((c) => `<td>${esc(c)}</td>`).join('')}</tr>`).join('')}</table>`;
    const rooms = live();
    const html = `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NFS4 Stats</title>
<style>body{margin:0;background:#030b3d;color:#eef2ff;font:13px/1.5 Verdana,Tahoma,sans-serif;padding:16px}main{max-width:960px;margin:0 auto}
h1{font:italic 900 28px "Arial Black",Arial,sans-serif;color:#ffd000;margin:0 0 4px;text-transform:uppercase}h2{font:italic 900 15px "Arial Black",Arial,sans-serif;color:#ffd000;text-transform:uppercase;margin:22px 0 6px;border-bottom:1px solid #ffcc0080}
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px}.c{background:#0a1a66;border:1px solid #2e3d8c;border-left:3px solid #ffd000;padding:8px 10px}.v{font:bold 22px Verdana,sans-serif}.l{color:#9fb0e6;font-size:11px}
table{border-collapse:collapse;width:100%;font-size:12px}th{text-align:left;color:#ffd000;font-size:10px;text-transform:uppercase;letter-spacing:.08em;padding:4px 8px;background:#0d1858}td{padding:4px 8px;border-top:1px solid #16205c}.m{color:#9fb0e6}.wrap{overflow-x:auto}.cols{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:640px){.cols{grid-template-columns:1fr}}</style>
<main><h1>Stats</h1><div class="m">Last 30 days · no cookies, visitor ids are hashed per day</div>
<h2>Overview</h2><div class="cards">${[
      card('visitors (per-day unique)', [...days.values()].reduce((n, d) => n + d.visitors.size, 0)),
      card('page views', views.length), card('game starts', starts.length), card('online game starts', by('online_start').length),
      card('rooms created', by('room_create').length), card('median load to first frame', loadS != null ? loadS.toFixed(1) + ' s' : '–'),
      card('median play time', playM != null ? playM.toFixed(1) + ' min' : '–'), card('median fps', fps != null ? Math.round(fps) : '–'), card('errors', by('error').length),
      card('racers online now', rooms.reduce((n, r) => n + r.players.length, 0)),
    ].join('')}</div>
<h2>Load phases</h2><div class="m">Median seconds since the page opened (download: how long the game download took). Left while loading: ${by('error').filter((r) => r.d.kind === 'left_loading').length}.</div>
<div class="wrap">${table(['phase', 'median', 'all loads', 'cached loads'], PHASES.map(([k, label]) => {
      const all = by('load').map((r) => r.d[k]).filter((x) => typeof x === 'number'), cached = by('load').filter((r) => r.d.cached).map((r) => r.d[k]).filter((x) => typeof x === 'number');
      return [label, median(all) != null ? median(all).toFixed(1) + ' s' : '–', all.length, median(cached) != null ? median(cached).toFixed(1) + ' s' : '–'];
    }))}</div>
<h2>By day</h2><div class="wrap">${table(['day', 'visitors', 'views', 'game starts', 'online starts', 'errors'], [...days].sort((a, b) => b[0].localeCompare(a[0])).map(([d, x]) => [d, x.visitors.size, x.views, x.starts, x.online, x.errors]))}</div>
<div class="cols"><div><h2>Browsers</h2>${table(['browser', 'views'], count(views, (r) => `${r.d.browser} ${r.d.version || ''} · ${r.d.os}`).slice(0, 12))}</div>
<div><h2>Screens</h2>${table(['viewport', 'views'], count(views, (r) => `${r.d.w}×${r.d.h}`).slice(0, 12))}</div></div>
<h2>Live rooms</h2><div class="wrap">${rooms.length ? table(['room', 'players', 'racing', 'password'], rooms.map((r) => [r.name, r.players.map((p) => p.name).join(', '), r.racing ? 'yes' : '', r.locked ? 'yes' : ''])) : '<p class="m">No rooms right now.</p>'}</div>
<h2>Recent errors</h2><div class="wrap">${table(['when', 'kind', 'detail', 'client log'], by('error').slice(-25).reverse().map((r) => [new Date(r.t).toISOString().replace('T', ' ').slice(0, 19), r.d.kind || '', r.d.msg || '', `build/clientlogs/${r.s}.log`]))}</div>
</main>`;
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store', 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' });
    res.end(html);
  }
  return { handleEvent, handleStats };
}
