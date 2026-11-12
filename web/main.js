// NFS4 High Stakes Web Runtime: page side.
//  1. The hosted NFS4 files are downloaded once into OPFS (navigator.storage, origin-private).
//  2. The wasm runtime (dist/nfs4.js, built from the server game executable)
//     mounts OPFS at /opfs and runs the game in Web Workers on shared memory.
//  3. Each animation frame we upload the shared framebuffer to WebGL; input goes
//     into a lock-free ring; audio comes out of a ring through an AudioWorklet.

const $ = (id) => document.getElementById(id);
const status = (msg, err = false) => { $('status').textContent = msg; $('status').className = err ? 'err' : ''; };

// on-page log (the console is hard to reach on some devices); ?log keeps it over the game
const t0 = performance.now();
// the log is hidden unless asked for: ?log in the URL, or "Show technical log" (remembered)
const logWanted = () => { try { return new URLSearchParams(location.search).has('log') || localStorage.getItem('nfs4.log') === '1'; } catch { return false; } };
function log(...a) {
  const el = $('log'); if (!el) return;
  const line = `${((performance.now() - t0) / 1000).toFixed(1).padStart(6)}  ${a.join(' ')}\n`;
  el.textContent = (el.textContent + line).slice(-20000);
  el.scrollTop = el.scrollHeight;
  logQueue.push(line);
  if (/ERROR|ABORT|stall/.test(line)) flushLog();
}
// every log line also goes to the server (build/clientlogs/<session>.log), so problems
// on a player's machine can be looked at afterwards
const logSession = new Date().toISOString().slice(0, 19).replace(/[-:T]/g, '') + '-' + Math.random().toString(36).slice(2, 8);
const logQueue = [`session ${logSession} ${navigator.userAgent} cores=${navigator.hardwareConcurrency} ${location.href}\n`];
function flushLog(beacon = false) {
  if (!logQueue.length) return;
  const body = logQueue.join(''); logQueue.length = 0;
  const url = `__log?s=${logSession}`;
  try { if (beacon) navigator.sendBeacon(url, body); else fetch(url, { method: 'POST', body, keepalive: body.length < 60000 }).catch(() => {}); } catch {}
}
setInterval(flushLog, 3000);

// page analytics (web/analytics.mjs on our own server: no cookies, no third parties)
function track(e, d = {}) {
  const body = JSON.stringify({ e, s: logSession, d });
  try { if (!navigator.sendBeacon('__event', body)) fetch('__event', { method: 'POST', body, keepalive: true }).catch(() => {}); } catch {}
}
const pageT0 = Date.now();
let playStats = { played: false, frames: 0, ms: 0 };
track('view', { w: innerWidth, h: innerHeight, lang: navigator.language, room_link: !!location.hash.match(/room=/) });
window.addEventListener('pagehide', () => track('end', { secs: (Date.now() - pageT0) / 1000, played: playStats.played, fps: playStats.ms ? playStats.frames * 1000 / playStats.ms : 0 }));
window.addEventListener('pagehide', () => flushLog(true));
// gave up while the game was loading (black screen, too slow): worth knowing, with the phases so far
window.addEventListener('pagehide', () => { if (phase.click != null && phase.shown == null) track('error', { kind: 'left_loading', msg: Object.entries(phase).map(([k, v]) => `${k}=${v}`).join(' '), secs: (performance.now() - t0) / 1000 }); });
for (const k of ['log', 'warn', 'error']) { const orig = console[k].bind(console); console[k] = (...a) => { orig(...a); log(...a.map(String)); }; }
window.addEventListener('error', (e) => log('ERROR', e.message, e.filename ? `${e.filename}:${e.lineno}` : ''));
window.addEventListener('unhandledrejection', (e) => log('ERROR (promise)', e.reason?.stack || e.reason));

import { Net } from './net.js';

const GAME_DIR = 'nfs4';
const net = new Net();
const roomFromHash = () => (new URLSearchParams(location.hash.slice(1)).get('room') || '').toLowerCase().replace(/[^a-z0-9-]/g, '');
const roomLink = (r) => `${location.origin}${location.pathname}#room=${r}`;
let buildInfo = null;

// ----------------------------------------------------------------- storage
async function gameDir(create = false) {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(GAME_DIR, { create });
}
async function hasGame() {
  try { const d = await gameDir(); await d.getFileHandle('.download-complete'); return true; } catch { return false; }
}
async function sha256(blob) {
  const h = await crypto.subtle.digest('SHA-256', await blob.arrayBuffer());
  return [...new Uint8Array(h)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// `node web/serve.mjs --game DIR`: the page copies that folder from the server (no folder picker)
async function serverHasGame() {
  try { const r = await fetch('__dev/game-list', { method: 'HEAD' }); return r.ok; } catch { return false; }
}
async function serverFiles() {
  const response = await fetch('__dev/game-list');
  if (!response.ok) throw new Error('Game download unavailable. Reload to retry.');
  const list = await response.json();
  return list.map(([path, size]) => ({
    name: path.split('/').at(-1), size, webkitRelativePath: 'dev/' + path,
    blob: () => fetch('__dev/game/' + path.split('/').map(encodeURIComponent).join('/')).then((r) => {
      if (!r.ok) throw new Error(`Download failed: ${path} (${r.status}). Reload to retry.`);
      return r.blob();
    }),
  }));
}

// Static game pack (tools/pack-game.mjs, hosted on Cloudflare R2 or any CDN with CORS).
// web/game-source.json = {"url": "https://files.example.com/nfs4/<id>/"}. The page reads
// <url>index.json, then streams each chunk-NNN.gz through the browser's gzip decoder
// straight into OPFS, file by file. Finished chunks are remembered, so a reload resumes
// with the next chunk instead of starting over.
async function packSource() {
  try {
    const r = await fetch('game-source.json', { cache: 'no-cache' });
    if (!r.ok) return null;
    const { url } = await r.json();
    return url ? url.replace(/\/?$/, '/') : null;
  } catch { return null; }
}
async function readText(dir, name) { try { return await (await (await dir.getFileHandle(name)).getFile()).text(); } catch { return null; } }
async function writeText(dir, name, text) { const w = await (await dir.getFileHandle(name, { create: true })).createWritable(); await w.write(text); await w.close(); }
async function importPack(base) {
  status('Checking the game download…');
  const ir = await fetch(base + 'index.json');
  if (!ir.ok) throw new Error(`Game download unavailable (${ir.status}). Reload to retry.`);
  const idx = await ir.json();
  if (idx.format !== 1) throw new Error('Unknown game pack format. Reload to retry.');
  if (buildInfo?.exe_sha256 && idx.exe_sha256 !== buildInfo.exe_sha256)
    throw new Error(`The hosted game files don't match this build (nfs4.exe ${idx.exe_sha256.slice(0, 12)}…, build expects ${buildInfo.exe_sha256.slice(0, 12)}…).`);
  const root = await navigator.storage.getDirectory();
  let dir = await gameDir(true);
  if ((await readText(dir, '.pack-id')) !== idx.id) {               // other or no earlier download: start clean
    await root.removeEntry(GAME_DIR, { recursive: true }).catch(() => {});
    dir = await gameDir(true);
    await writeText(dir, '.pack-id', idx.id);
  }
  const doneChunks = new Set(((await readText(dir, '.pack-done')) || '').split(',').filter(Boolean));
  const dirs = new Map([['', dir]]);
  const subdir = async (path) => {
    const parts = path.split('/').slice(0, -1);
    let key = '', d = dir;
    for (const p of parts) { key += p + '/'; if (!dirs.has(key)) dirs.set(key, await d.getDirectoryHandle(p, { create: true })); d = dirs.get(key); }
    return d;
  };
  const MB = 2 ** 20, t0 = performance.now();
  let got = idx.chunks.filter((c) => doneChunks.has(c.name)).reduce((s, c) => s + c.gz, 0), fresh = 0;
  $('progress').hidden = false;
  const show = () => {
    $('progress').value = got / idx.gz;
    const secs = (performance.now() - t0) / 1000, rate = fresh / Math.max(secs, 0.5);
    status(`Downloading the game files… ${(got / MB).toFixed(0)} / ${(idx.gz / MB).toFixed(0)} MB` + (secs > 2 && rate > 0 ? ` · ${(rate / MB).toFixed(1)} MB/s` : ''));
  };
  if (got) log(`game pack ${idx.id}: resuming, ${doneChunks.size}/${idx.chunks.length} chunks already here`);
  for (const c of idx.chunks) {
    if (doneChunks.has(c.name)) continue;
    let lastErr;
    for (let attempt = 0; attempt < 4; attempt++) {
      const before = got, beforeFresh = fresh;
      try {
        const r = await fetch(base + c.name);
        if (!r.ok) throw new Error(`${c.name}: HTTP ${r.status}`);
        const counted = r.body.pipeThrough(new TransformStream({ transform(b, ctl) { got += b.length; fresh += b.length; show(); ctl.enqueue(b); } }));
        const rd = counted.pipeThrough(new DecompressionStream('gzip')).getReader();
        let buf = new Uint8Array(0);                          // decoded bytes not yet written
        const pull = async () => { const { done, value } = await rd.read(); if (done) throw new Error(`${c.name}: ended early`); buf = buf.length ? concat(buf, value) : value; };
        for (const [path, size] of c.files) {
          const fh = await (await subdir(path)).getFileHandle(path.split('/').at(-1), { create: true });
          const isIni = path.toLowerCase() === 'nfs4.ini', ini = [];
          const w = await fh.createWritable();
          let left = size;
          while (left > 0) {
            if (!buf.length) await pull();
            const n = Math.min(left, buf.length), part = buf.subarray(0, n);
            if (isIni) ini.push(part.slice()); else await w.write(exact(part));
            buf = buf.subarray(n); left -= n;
          }
          if (isIni) await w.write(patchIni(new TextDecoder('latin1').decode(concat(...ini))));
          await w.close();
        }
        doneChunks.add(c.name);
        await writeText(dir, '.pack-done', [...doneChunks].join(','));
        lastErr = null; break;
      } catch (e) {
        lastErr = e; got = before; fresh = beforeFresh;
        log(`game pack: ${c.name} failed (${e.message}), attempt ${attempt + 1}`);
        await new Promise((r) => setTimeout(r, 1500 * (attempt + 1)));
      }
    }
    if (lastErr) throw new Error(`Download failed: ${lastErr.message}. Reload to continue where it stopped.`);
  }
  for (const d of idx.dirs || []) await subdir(d + '/');
  log(`game pack ${idx.id}: ${(fresh / MB).toFixed(0)} MB in ${((performance.now() - t0) / 1000).toFixed(0)} s`);
  await writeText(dir, '.download-complete', 'complete');
  $('progress').hidden = true;
}
// Safari's FileSystemWritableFileStream.write() writes the whole ArrayBuffer behind a typed array,
// ignoring a subarray's offset and length, so only ever hand it arrays that cover their buffer.
const exact = (u) => (u.byteOffset === 0 && u.byteLength === u.buffer.byteLength ? u : u.slice());
// Downloads made by Safari before that fix have files of the wrong size: compare every file with
// the pack index once per pack, and download again if any is off (files the page or the game
// rewrites, nfs4.ini, TEXT.ENG and the saves, are left out).
async function checkPack() {
  try {
    if (!(await hasGame())) return;
    const d = await gameDir(), id = await readText(d, '.pack-id');
    if (!id || (await readText(d, '.pack-checked')) === id) return;
    const base = await packSource();
    if (!base) return;
    const idx = await (await fetch(base + 'index.json')).json();
    if (idx.id !== id) return;
    let bad = 0, first = '';
    for (const c of idx.chunks) for (const [path, size] of c.files) {
      if (/^(nfs4\.ini|data\/text\/text\.eng|savedata\/.*)$/i.test(path)) continue;
      const parts = path.split('/');
      let ok = false;
      try { let h = d; for (const p of parts.slice(0, -1)) h = await h.getDirectoryHandle(p); ok = (await (await h.getFileHandle(parts.at(-1))).getFile()).size === size; } catch {}
      if (!ok) { bad++; first ||= path; }
    }
    if (bad) {
      log(`game files: ${bad} damaged (first: ${first}), downloading them again`);
      track('error', { kind: 'damaged', msg: `${bad} files` });
      for (const f of ['.download-complete', '.pack-done']) await d.removeEntry(f).catch(() => {});
      try { const t = await ciDir(await ciDir(d, 'data'), 'text'); await t.removeEntry(await ciFile(t, 'text.eng.orig')); } catch {}
    } else await writeText(d, '.pack-checked', id);
  } catch (e) { log('game files check failed:', e.message); }
}
function concat(...parts) {
  const out = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
  let o = 0; for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

// ------------------------------------------------------------------ names
// One name everywhere: the lobby, the relay (relay.mjs cleanName) and the game. The game keeps
// 10 characters and only has glyphs for plain letters, digits and a few signs.
const NAME_MAX = 10;
const cleanName = (v) => String(v || '').replace(/[^A-Za-z0-9 ._!-]/g, '').replace(/\s+/g, ' ').trimStart().slice(0, NAME_MAX);
const randomName = () => 'Player' + Math.floor(1000 + Math.random() * 9000);
// Before the game starts, put the name where the game reads it:
//  - data/TEXT/TEXT.ENG: the default "Player1" (a string table: u32 offsets, then NUL-terminated
//    strings) that the game's name dialog offers until it has saved its own settings. Rebuilt each
//    time from the untouched copy TEXT.ENG.orig, so names never pile up.
//  - savedata/config.dat, once the game has saved it: the player name field (16 bytes at 0xF2C).
// OPFS names keep the case of the hosted files, so every lookup here ignores case.
async function ciDir(dir, name) { for await (const [n, h] of dir.entries()) if (h.kind === 'directory' && n.toLowerCase() === name.toLowerCase()) return h; return null; }
async function ciFile(dir, name) { for await (const [n, h] of dir.entries()) if (h.kind === 'file' && n.toLowerCase() === name.toLowerCase()) return n; return name; }
async function writeGameName(name) {
  name = cleanName(name).trim();
  if (!name) return;
  const latin = new TextEncoder();
  try {
    const text = await ciDir(await ciDir(await gameDir(), 'data'), 'text');
    const eng = await ciFile(text, 'text.eng'), bak = eng + '.orig';
    let orig = await fileBytes(text, bak);
    if (!orig) { orig = await fileBytes(text, eng); if (orig) await writeBytes(text, bak, orig); }
    if (orig) {
      const dv = new DataView(orig.buffer, orig.byteOffset, orig.byteLength), n = dv.getUint32(0, true) / 4;
      const strs = [];
      for (let i = 0; i < n; i++) { const o = dv.getUint32(i * 4, true); let e = o; while (e < orig.length && orig[e]) e++; strs.push(orig.subarray(o, e)); }
      const p1 = latin.encode('Player1'), nm = latin.encode(name);
      const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
      const out = strs.map((x) => (eq(x, p1) ? nm : x));
      const size = n * 4 + out.reduce((t, x) => t + x.length + 1, 0), buf = new Uint8Array(size), odv = new DataView(buf.buffer);
      let o = n * 4;
      out.forEach((x, i) => { odv.setUint32(i * 4, o, true); buf.set(x, o); o += x.length + 1; });
      await writeBytes(text, eng, buf);
    }
  } catch (e) { log('name: TEXT.ENG not patched:', e.message); }
  try {
    const save = await ciDir(await gameDir(), 'savedata');
    const cfgName = await ciFile(save, 'config.dat'), cfg = await fileBytes(save, cfgName);
    const o = 0xf2c;
    if (cfg && cfg.length >= o + 16) {
      let k = 0, ok = true;
      while (k < 16 && cfg[o + k]) { if (cfg[o + k] < 32 || cfg[o + k] > 126) ok = false; k++; }
      if (ok && k > 0 && k < 16) { cfg.fill(0, o, o + 16); cfg.set(latin.encode(name), o); await writeBytes(save, cfgName, cfg); }
    }
  } catch {}
  log(`name: the game will call you "${name}"`);
}
async function fileBytes(dir, name) { try { return new Uint8Array(await (await (await dir.getFileHandle(name)).getFile()).arrayBuffer()); } catch { return null; } }
async function writeBytes(dir, name, bytes) { const w = await (await dir.getFileHandle(name, { create: true })).createWritable(); await w.write(exact(bytes)); await w.close(); }

// The browser build renders through the Voodoo2 (Glide) driver -> WebGL2, and skips the FMVs
// (intro, menu attract demos): the game's movie streamer races its file reader thread and on
// multi-core machines can wait forever for the first frame (about 1 start in 4 natively, nearly
// every start in Safari). NoMovies is the modern patch's own switch for this.
const iniSet = (t, key, val) => (new RegExp(`^${key}\\s*=.*$`, 'mi').test(t) ? t.replace(new RegExp(`^${key}\\s*=.*$`, 'mi'), `${key}=${val}`) : t);
const patchIni = (t) => iniSet(iniSet(iniSet(t, 'ThrashDriver', 'glide3x'), 'NoMovies', '1'), 'SingleProcAffinity', '1');
async function ensureIni() {
  try {
    const d = await gameDir();
    const fh = await d.getFileHandle('nfs4.ini');
    const t = await (await fh.getFile()).text(), u = patchIni(t);
    if (u !== t) { const w = await fh.createWritable(); await w.write(u); await w.close(); log('nfs4.ini: ThrashDriver=glide3x, NoMovies=1, SingleProcAffinity=1'); }
  } catch (e) { log('nfs4.ini not patched:', e.message); }
}

async function importFolder(files) {
  // webkitRelativePath = "<picked folder>/sub/file"; drop the first component.
  const list = [...files].filter((f) => {
    const name = f.name;
    return !name.startsWith('._') && name !== '.DS_Store' && f.webkitRelativePath.split('/').length > 1;
  });
  const rel = (f) => f.webkitRelativePath.split('/').slice(1).join('/');
  const exe = list.find((f) => rel(f).toLowerCase() === 'nfs4.exe');
  if (!exe) throw new Error('No nfs4.exe in that folder. Pick the folder that contains nfs4.exe, data and savedata.');
  if (buildInfo?.exe_sha256) {
    status('Checking nfs4.exe…');
    const h = await sha256(exe.blob ? await exe.blob() : exe);
    if (h !== buildInfo.exe_sha256)
      throw new Error(`This build was recompiled from the NFS4 Modern Patch beta nfs4.exe (veg.by/files/nfs4/nfs4_modern_patch_beta.7z); yours is different (expected ${buildInfo.exe_sha256.slice(0, 12)}…, got ${h.slice(0, 12)}…). Install that patch into your game folder and pick it again.`);
  }
  const root = await navigator.storage.getDirectory();
  try { await root.removeEntry(GAME_DIR, { recursive: true }); } catch {}
  const dir = await gameDir(true);
  const total = list.reduce((s, f) => s + f.size, 0);
  let done = 0;
  $('progress').hidden = false;
  // folders that differ only in case (a CD's data/MENUS and the patch's data/menus) are one folder
  // to the game, so merge them here; a later file of the same name (the patch's) wins
  const dirs = new Map([['', dir]]);
  for (const f of list) {
    const parts = rel(f).split('/');
    let d = dir, key = '';
    for (const p of parts.slice(0, -1)) {
      key += p.toLowerCase() + '/';
      if (!dirs.has(key)) dirs.set(key, (await ciDir(d, p)) || await d.getDirectoryHandle(p, { create: true }));
      d = dirs.get(key);
    }
    const fh = await d.getFileHandle(await ciFile(d, parts.at(-1)), { create: true });
    const src = f.blob ? await f.blob() : f;
    const w = await fh.createWritable();
    if (parts.length === 1 && parts[0].toLowerCase() === 'nfs4.ini') {
      const ini = patchIni(await src.text());
      await w.write(ini);
    } else {
      await w.write(src);
    }
    await w.close();
    done += f.size;
    $('progress').value = done / total;
    status(`Copying game files… ${(done / 2 ** 20).toFixed(0)} / ${(total / 2 ** 20).toFixed(0)} MB`);
  }
  const marker = await (await dir.getFileHandle('.download-complete', { create: true })).createWritable();
  await marker.write('complete'); await marker.close();
  $('progress').hidden = true;
}

// ------------------------------------------------------------------- video
// The recompiled game renders through Glide -> our GL ES 3 renderer (runtime/gpu.c),
// which draws into this canvas with WebGL2 when we call _rt_gpu_frame each frame.
function fitCanvas(canvas) {
  const dpr = devicePixelRatio || 1;
  const cw = Math.round(canvas.clientWidth * dpr), ch = Math.round(canvas.clientHeight * dpr);
  if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
}

// ------------------------------------------------------------------- input
// KeyboardEvent.code -> runtime key names (runtime/display.c key table)
const CODE_TO_KEY = {
  Escape: 'ESCAPE', Enter: 'RETURN', NumpadEnter: 'RETURN', Space: 'SPACE', Tab: 'TAB', Backspace: 'BACKSPACE',
  ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT',
  ShiftLeft: 'LSHIFT', ShiftRight: 'RSHIFT', ControlLeft: 'LCTRL', ControlRight: 'RCTRL', AltLeft: 'LALT', AltRight: 'RALT',
  Insert: 'INSERT', Delete: 'DELETE', Home: 'HOME', End: 'END', PageUp: 'PAGEUP', PageDown: 'PAGEDOWN',
};
for (let i = 1; i <= 12; i++) CODE_TO_KEY['F' + i] = 'F' + i;
for (let c = 65; c <= 90; c++) CODE_TO_KEY['Key' + String.fromCharCode(c)] = String.fromCharCode(c);
for (let d = 0; d <= 9; d++) CODE_TO_KEY['Digit' + d] = String(d);

// ------------------------------------------------------------- fullscreen
// The whole page goes fullscreen (the game, the fps and online badges stay on top). In Chrome the
// keyboard lock keeps Esc for the game while fullscreen (hold Esc to leave); elsewhere Esc leaves.
const fsElement = () => document.fullscreenElement || document.webkitFullscreenElement || null;
const fsSupported = () => !!(document.fullscreenEnabled || document.webkitFullscreenEnabled);
async function toggleFullscreen() {
  try {
    if (fsElement()) { await (document.exitFullscreen || document.webkitExitFullscreen).call(document); return; }
    const el = document.documentElement;
    await (el.requestFullscreen ? el.requestFullscreen({ navigationUI: 'hide' }) : el.webkitRequestFullscreen());
  } catch (e) { log('fullscreen:', e.message || e); }
}
function setupFullscreen() {
  const b = $('fullscreen');
  if (!fsSupported()) return;                 // iPhone Safari: only videos can go fullscreen
  b.hidden = false;
  b.addEventListener('click', (ev) => { ev.stopPropagation(); b.blur(); toggleFullscreen(); });
  const changed = () => {
    const on = !!fsElement();
    b.title = on ? 'Exit fullscreen (Alt+Enter)' : 'Fullscreen (Alt+Enter)';
    b.setAttribute('aria-label', b.title);
    document.body.classList.toggle('fs', on);
    const kb = on && navigator.keyboard?.lock;
    if (kb) navigator.keyboard.lock(['Escape']).catch(() => {}); else navigator.keyboard?.unlock?.();
    $('hint').textContent = 'Click to capture the mouse · ' + (kb ? 'hold Esc to leave fullscreen' : on ? 'Esc releases it and leaves fullscreen' : 'Esc releases it · Alt+Enter fullscreen');
  };
  document.addEventListener('fullscreenchange', changed);
  document.addEventListener('webkitfullscreenchange', changed);
}

function attachInput(mod, canvas) {
  const base = mod._rt_web_input();
  const QN = 1024;
  const heap = () => new Int32Array(mod.HEAPU8.buffer);
  const keyIndex = {};
  for (const [code, name] of Object.entries(CODE_TO_KEY)) keyIndex[code] = mod.ccall('rt_web_key_index', 'number', ['string'], [name]);

  function push(type, a, b) {
    const h32 = heap(), hi = base >> 2;
    const head = Atomics.load(h32, hi), tail = Atomics.load(h32, hi + 1);
    if (((head - tail) >>> 0) >= QN) return;                    // full: drop
    const e = hi + 2 + (head % QN) * 4;
    h32[e] = type; h32[e + 1] = a; h32[e + 2] = b; h32[e + 3] = 0;
    Atomics.store(h32, hi, (head + 1) | 0);
  }
  const held = new Set();
  const onKey = (down) => (ev) => {
    const idx = keyIndex[ev.code];
    if (idx === undefined || idx < 0) return;
    ev.preventDefault();
    if (down && held.has(ev.code) && !ev.repeat) return;
    if (down) held.add(ev.code); else held.delete(ev.code);
    push(1, idx, down ? 1 : 0);
  };
  // Alt+Enter toggles fullscreen; the game never sees that Enter (down or up)
  let fsEnter = false;
  window.addEventListener('keydown', (ev) => {
    if (ev.code === 'Enter' && ev.altKey) { ev.preventDefault(); ev.stopImmediatePropagation(); if (!ev.repeat) { fsEnter = true; toggleFullscreen(); } }
  }, true);
  window.addEventListener('keyup', (ev) => { if (ev.code === 'Enter' && fsEnter) { fsEnter = false; ev.preventDefault(); ev.stopImmediatePropagation(); } }, true);
  window.addEventListener('keydown', onKey(true));
  window.addEventListener('keyup', onKey(false));
  window.addEventListener('blur', () => { for (const c of held) push(1, keyIndex[c], 0); held.clear(); });

  // the game's cursor is relative (DirectInput): pointer lock gives clean deltas
  const acc = { x: 0, y: 0 };
  canvas.addEventListener('click', () => { if (document.pointerLockElement !== canvas) canvas.requestPointerLock?.(); });
  document.addEventListener('pointerlockchange', () => {
    const locked = document.pointerLockElement === canvas;
    $('hint').hidden = locked; document.body.classList.toggle('locked', locked);
  });
  canvas.addEventListener('mousemove', (ev) => {
    if (document.pointerLockElement !== canvas) return;
    // send deltas in 640x480 game coordinates (the runtime applies the game's sensitivity correction)
    const s = 640 / canvas.clientWidth;
    acc.x += ev.movementX * s; acc.y += ev.movementY * s;
    const dx = Math.trunc(acc.x), dy = Math.trunc(acc.y);
    acc.x -= dx; acc.y -= dy;
    if (dx || dy) push(2, dx, dy);
  });
  canvas.addEventListener('mousedown', (ev) => { if (document.pointerLockElement === canvas) push(3, ev.button === 2 ? 3 : 1, 1); });
  canvas.addEventListener('mouseup', (ev) => { if (document.pointerLockElement === canvas) push(3, ev.button === 2 ? 3 : 1, 0); });
  canvas.addEventListener('wheel', (ev) => { push(4, ev.deltaY < 0 ? 120 : -120, 0); ev.preventDefault(); }, { passive: false });
  canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());
}

// ------------------------------------------------------------------- audio
// The AudioContext is made inside the Start click (Safari only lets audio start during the
// click itself, not after the awaits in start()); startAudio() then plugs the game into it.
let earlyAudio = null;
function makeAudioContext() {
  try {
    const ctx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
    ctx.resume().catch(() => {});
    return { ctx, worklet: ctx.audioWorklet.addModule('audio-worklet.js') };
  } catch (e) { log('audio context failed:', e.message); return null; }
}
async function startAudio(mod) {
  const a = earlyAudio || makeAudioContext();
  if (!a) return null;
  const ctx = a.ctx;
  await a.worklet;
  const node = new AudioWorkletNode(ctx, 'ring-player', { outputChannelCount: [2] });
  node.port.postMessage({ buffer: mod.HEAPU8.buffer, ptr: mod._rt_web_audio(), beat: mod._rt_web_heartbeat() });
  node.port.onmessage = (e) => { const { under, secs } = e.data; if (under > 0.05 * 44100 * secs) log(`audio: ${(under / 441 / secs).toFixed(0)}% silence over ${secs.toFixed(0)} s (game mixed too little)`); };
  node.connect(ctx.destination);
  log('audio context:', ctx.state);
  ctx.onstatechange = () => log('audio context:', ctx.state);
  // browsers keep audio suspended until a user gesture: never wait for it here
  if (ctx.state !== 'running') {
    log('audio waits for a click or key press');
    const resume = () => ctx.resume().then(() => { for (const e of ['pointerdown', 'keydown']) window.removeEventListener(e, resume, true); });
    for (const e of ['pointerdown', 'keydown']) window.addEventListener(e, resume, true);
  }
  return ctx;
}

function screenPixels() {
  const dpr = devicePixelRatio || 1, a = Math.round(screen.width * dpr), b = Math.round(screen.height * dpr);
  return `${Math.max(a, b)}x${Math.min(a, b)}`;
}
// Exit Game in the menu: the game can't start again in this page (its threads are done),
// so leave fullscreen and reload: the start page, with the game files already in place
async function backToPage() {
  gameDead = true;
  try { document.exitPointerLock?.(); } catch {}
  try { if (fsElement()) await (document.exitFullscreen || document.webkitExitFullscreen).call(document); } catch {}
  location.reload();
}
// the game ended (crash or exit): say so over the game instead of leaving a frozen picture
let gameDead = false;
function gameStopped(msg) {
  gameDead = true;
  status(msg, true);
  const el = $('log'); el.hidden = false; el.classList.add('overlay');
  log(msg); flushLog();
}

// -------------------------------------------------------------------- boot
// load phases (seconds since the page opened), sent with the 'load' event and shown on /__stats:
// files = game files ready, click = Start pressed, wasm = runtime running, first = first frame,
// shown = the loading cover lifted (the game is really drawing)
const phase = {};
const mark = (k) => { if (phase[k] == null) phase[k] = Math.round(performance.now() / 100) / 10; };

async function start() {
  $('start').disabled = true;
  mark('click');
  status('Loading the recompiled game…');
  await ensureIni();
  await writeGameName($('pname')?.value);
  const startT = performance.now();
  const dev = await deviceInfo();
  log('device:', Object.entries(dev).map(([k, v]) => `${k}=${v}`).join(' '));
  if (dev.software) log('WARNING: the browser renders without the GPU (hardware acceleration is off or blocked); expect low frame rates');
  track('start', { online: !!net.id, gpu: String(dev.gpu || ''), dpr: dev.dpr, cores: dev.cores, software: !!dev.software });
  if (net.id) track('online_start', { players: net.peers.size + 1 });
  if (net.id) log(`online: room ${net.room} "${net.info?.name}", ${net.peers.size + 1} player(s), node ${net.id}`);
  log('loading dist/nfs4.js');
  const { default: NFS4 } = await import('./dist/nfs4.js');
  log('instantiating wasm + starting worker pool (cores:', navigator.hardwareConcurrency + ')');
  const mod = await NFS4({
    setStatus: (t) => t && log('[emscripten]', t),
    monitorRunDependencies: (n) => log('[emscripten] pending dependencies:', n),
    onAbort: (what) => { log('ABORT', what); track('error', { kind: 'abort', msg: String(what) }); gameStopped('The game stopped: ' + what); },
    onGameMessage: (t) => { log('ERROR game message:', t); track('error', { kind: 'message', msg: t }); },
    onGameExit: (code) => {
      if (code === 0) { log('game quit: back to the start page'); backToPage(); return; }
      log('ERROR game exited, code', code); track('error', { kind: 'exit', msg: 'code ' + code }); gameStopped('The game closed itself (see the log). Reload the page to start again.');
    },
    // the screen in pixels, landscape: the game offers the resolutions that fit (glide.c)
    arguments: ['/opfs/nfs4', `RT_SCREEN=${screenPixels()}`],
    print: (t) => console.log(t),
    printErr: (t) => console.log(t),
  });
  log('runtime ready, game thread running');
  mark('wasm');
  if (net.id) { mod.ccall('rt_net_set_node', null, ['string'], [net.id]); net.attach(mod); net.setState('game'); showNetBadge(); }
  startPerfLog(mod);
  // keep the log over the game until frames arrive (always with ?log)
  const keepLog = logWanted();
  $('log').classList.add('overlay'); document.body.appendChild($('log'));
  $('log').hidden = !keepLog;
  gameStarted = true;
  $('setup-wrap').hidden = true; $('bg').hidden = true;
  $('screen').style.display = 'block';
  $('hint').hidden = false;
  setupFullscreen();
  // Loading cover: up until the game has really drawn a few frames (the first ones are an
  // empty screen), then a small "Loading…" badge whenever no new frame comes for 1.5 s
  // (the game loading a track, or stuck). This is the page's own text, never the game's.
  const cover = $('loading'), badge = $('loadbadge');
  cover.hidden = false;
  const canvas = $('canvas');
  attachInput(mod, canvas);
  startAudio(mod).catch((e) => log('audio unavailable:', e.message || e));

  let fpsN = 0, fpsT = performance.now(), first = true, stalled = false, lastFresh = performance.now();
  $('fps').hidden = false;
  function frame() {
    if (gameDead) return;
    fitCanvas(canvas);
    const m0 = performance.now();
    const fresh = mod._rt_gpu_frame(canvas.width, canvas.height);
    const mt = performance.now() - m0; hitch.mainMs += mt; hitch.mainMax = Math.max(hitch.mainMax, mt);
    if (fresh < 0) { status('WebGL2 is not available in this browser.', true); return; }
    if (fresh) {
      if (stalled) log(`stall over after ${((performance.now() - lastFresh) / 1000).toFixed(1)} s`);
      stalled = false; lastFresh = performance.now();
      fpsN++;
      if (first) { first = false; mark('first'); log('first frame'); track('first_frame', { secs: (performance.now() - startT) / 1000 }); playStats.played = true; }
      playStats.frames++;
      if (!cover.hidden && playStats.frames >= 8) {
        cover.hidden = true; mark('shown');
        track('load', { ...phase, cached: !phase.download });
        log('load phases:', Object.entries(phase).map(([k, v]) => `${k}=${v}s`).join(' '));
      }
      badge.hidden = true;
    }
    if (cover.hidden && document.visibilityState === 'visible' && performance.now() - lastFresh > 1500) badge.hidden = false;
    if (!cover.hidden && performance.now() - startT > 30000) $('loading-text').textContent = 'Still loading… if this stays black, reload the page.';
    const now = performance.now();
    if (first && !stalled && now - lastFresh > 30000) { stalled = true; log('stall: no first frame 30 s after start'); }
    if (!first && !stalled && now - lastFresh > 5000 && document.visibilityState === 'visible') {
      stalled = true; log('stall: no new frame from the game for 5 s'); track('error', { kind: 'stall', msg: 'no frame for 5 s' });
    }
    if (!first) playStats.ms += now - (frame.last ?? now);
    if (frame.last) { hitch.raf = Math.max(hitch.raf, now - frame.last); hitch.intervals.push(now - frame.last); }
    if (fresh && hitch.lastFresh) hitch.game = Math.max(hitch.game, now - hitch.lastFresh);
    if (fresh) { hitch.lastFresh = now; hitch.frames++; }
    frame.last = now;
    if (now - fpsT >= 1000) { $('fps').textContent = `${Math.round(fpsN * 1000 / (now - fpsT))} fps`; fpsN = 0; fpsT = now; }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// This browser's graphics and device, logged once at start (and sent with the start event),
// so a slow machine can be told apart from slow code: software rendering, a weak GPU,
// a high-DPI screen, battery saving...
async function deviceInfo() {
  const d = { cores: navigator.hardwareConcurrency || 0, mem: navigator.deviceMemory || 0, dpr: devicePixelRatio || 1,
    screen: `${screen.width}x${screen.height}`, window: `${innerWidth}x${innerHeight}`, isolated: crossOriginIsolated };
  try {
    const gl = document.createElement('canvas').getContext('webgl2', { powerPreference: 'high-performance' });
    if (!gl) d.gpu = 'no WebGL2';
    else {
      const ext = gl.getExtension('WEBGL_debug_renderer_info');
      d.gpu = ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
      d.vendor = ext ? gl.getParameter(ext.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR);
      d.maxtex = gl.getParameter(gl.MAX_TEXTURE_SIZE);
      d.software = /SwiftShader|llvmpipe|softpipe|Basic Render|Software/i.test(d.gpu);
      gl.getExtension('WEBGL_lose_context')?.loseContext();
    }
  } catch (e) { d.gpu = 'error ' + e.message; }
  try { const b = await navigator.getBattery?.(); if (b) d.battery = `${Math.round(b.level * 100)}%${b.charging ? ' charging' : ' on battery'}`; } catch {}
  try { const ua = await navigator.userAgentData?.getHighEntropyValues?.(['platformVersion', 'architecture', 'fullVersionList']);
    if (ua) d.platform = `${ua.platform} ${ua.platformVersion || ''} ${ua.architecture || ''}`.trim(); } catch {}
  return d;
}

// Perf log: every 2 s while the game runs, this browser's frame pacing (frames shown, frames
// the game made, time the game waited for the screen, render scale) and, online, one part per
// peer (link, round trip, packets, losses, longest gap between packets). It goes to the server
// with the rest of the log (build/clientlogs/), so lag can be looked at afterwards.
//
// It also lowers the render scale when the GPU can't keep up: the game draws 640x480 and we
// scale that by a whole number to fill the screen, which on a big or high-DPI screen means
// rendering up to 2560x1920. If frames are ready but the screen doesn't take them in time,
// the next lower scale is used (and remembered in this browser). ?scale=N forces one.
const hitch = { raf: 0, game: 0, frames: 0, lastFresh: 0, mainMs: 0, mainMax: 0, intervals: [] };
const savedScale = () => { try { return Number(new URLSearchParams(location.search).get('scale')) || Number(localStorage.getItem('nfs4.maxScale')) || 3; } catch { return 3; } };
function startPerfLog(mod) {
  if (net.id) log(`net: room ${net.room} (${net.info?.name || ''}) as "${$('pname')?.value || ''}" id ${net.id}`);
  const forced = (() => { try { return new URLSearchParams(location.search).has('scale'); } catch { return false; } })();
  let maxScale = savedScale(), slow = 0;
  mod._rt_gpu_set_max_scale(maxScale);
  log(`perf: render scale limit ${maxScale}${forced ? ' (forced by ?scale)' : ''}`);
  const stats = () => { const p = mod._rt_gpu_stats() >> 3; return Array.from(mod.HEAPF64.subarray(p, p + 6)); };
  let t = performance.now(), prev = stats();
  setInterval(() => {
    const now = performance.now(), secs = (now - t) / 1000; t = now;
    const cur = stats(), [pub, rep, wait] = cur.map((v, i) => v - prev[i]); prev = cur;
    const [, , , scale, fw, fh] = cur;
    const shown = hitch.frames / secs, made = pub / secs, waitShare = wait / (secs * 1000);
    const iv = hitch.intervals.sort((a, b) => a - b), hz = iv.length > 10 ? 1000 / iv[Math.floor(iv.length * 0.1)] : 60;
    const visible = document.visibilityState === 'visible';
    if (visible && shown > 5) { net.myFps = Math.round(shown); try { localStorage.setItem('nfs4.lastFps', String(net.myFps)); } catch {} }
    log(`perf ${visible ? '' : '[tab hidden] '}fps=${Math.round(shown)} made=${Math.round(made)} replaced=${rep} wait=${Math.round(waitShare * 100)}% hz=${Math.round(hz)}`
      + ` maxframe=${Math.round(hitch.game)}ms raf=${Math.round(hitch.raf)}ms main=${(hitch.mainMs / Math.max(1, iv.length)).toFixed(1)}/${Math.round(hitch.mainMax)}ms scale=${scale} (${fw}x${fh})`
      + (net.id ? ' | ' + (net.netStats().join(' | ') || 'no peers') : ''));
    // frames are waiting for the screen, but the screen runs well below its refresh rate: GPU-bound
    const gpuBound = visible && hitch.lastFresh && shown < hz * 0.9 && (waitShare > 0.2 || rep > secs * 3);
    slow = gpuBound ? slow + 1 : 0;
    if (!forced && slow >= 2 && scale > 1) {
      maxScale = scale - 1; slow = 0;
      mod._rt_gpu_set_max_scale(maxScale);
      try { localStorage.setItem('nfs4.maxScale', String(maxScale)); } catch {}
      log(`perf: GPU can't keep up (${Math.round(shown)} fps at ${fw}x${fh}), render scale lowered to ${maxScale}`);
    }
    Object.assign(hitch, { raf: 0, game: 0, frames: 0, mainMs: 0, mainMax: 0, intervals: [] });
  }, 2000);
  document.addEventListener('visibilitychange', () => log(`perf: tab ${document.visibilityState}`));
}

function showNetBadge() {
  const b = $('netbadge');
  const draw = () => {
    const s = net.summary();
    b.textContent = `${s.name || 'Room ' + s.room} · ${s.players} player${s.players === 1 ? '' : 's'}` + (s.peers.length ? ' · ' + s.peers.join(' · ') : ' · waiting for friends')
      + (s.best ? ` · best host: ${s.best.name}${s.best.me ? ' (create the game)' : ''}` : '');
  };
  b.hidden = false; draw();
  net.addEventListener('status', draw);
  b.onclick = () => navigator.clipboard?.writeText(roomLink(net.room)).then(() => { b.textContent = 'Room link copied'; setTimeout(draw, 1200); });
}

// ------------------------------------------------------------------ lobby
// Room browser and room page. Rooms live on the relay (relay.mjs); we join
// before Start so everyone sees each other (and WebRTC links come up) early.
const el = (tag, props = {}, ...kids) => { const e = Object.assign(document.createElement(tag), props); e.append(...kids); return e; };
const pingClass = (ms) => (ms < 60 ? 'ping-good' : ms < 130 ? 'ping-ok' : 'ping-bad');
const savedPass = (code) => { try { return sessionStorage.getItem('nfs4.pass.' + code) || ''; } catch { return ''; } };
const savePass = (code, pw) => { try { pw ? sessionStorage.setItem('nfs4.pass.' + code, pw) : sessionStorage.removeItem('nfs4.pass.' + code); } catch {} };
let gameStarted = false;

function setupOnline() {
  const name = $('pname');
  // a name from the start: the saved one, else a random Player1234 (everyone being "Player"
  // makes the in-game lists useless)
  try { name.value = cleanName(localStorage.getItem('nfs4.name')).trim(); } catch {}
  if (!name.value) { name.value = randomName(); try { localStorage.setItem('nfs4.name', name.value); } catch {} }
  name.addEventListener('input', () => {
    const c = cleanName(name.value); if (c !== name.value) name.value = c;
    try { localStorage.setItem('nfs4.name', name.value.trim()); } catch {}
  });
  const myName = () => cleanName(name.value).trim();
  const needName = () => {
    if (myName()) return false;
    message('Enter your name first.'); name.focus(); return true;
  };
  const message = (t) => { $('lobby-msg').textContent = t || ''; $('lobby-msg').hidden = !t; };
  const server = (t, cls = '') => { $('server-state').textContent = t; $('server-state').className = 'pill ' + cls; };
  let joining = null;          // room code waiting for a password

  // ---- room list
  function drawRooms() {
    const tb = $('rooms').tBodies[0]; tb.textContent = '';
    const racers = String(Math.min(999, net.rooms.reduce((n, r) => n + r.players.length, 0))).padStart(3, '0');
    $('online-count').replaceChildren(...[...racers].map((d) => el('i', { textContent: d })));
    const rooms = [...net.rooms].sort((a, b) => (a.racing - b.racing) || (b.players.length - a.players.length) || (b.created - a.created));
    $('rooms-empty').hidden = rooms.length > 0; $('rooms').hidden = !rooms.length;
    for (const r of rooms) {
      const full = r.players.length >= r.max;
      const rtts = r.players.map((p) => p.rtt).filter((x) => x != null);
      const ping = net.relayRtt != null && rtts.length ? Math.round(net.relayRtt + rtts.reduce((a, b) => a + b, 0) / rtts.length) : null;
      const title = el('div', { className: 'rname', textContent: r.name });
      if (r.locked) title.append(el('span', { className: 'tag', textContent: 'password', title: 'Needs a password' }));
      if (r.racing) title.append(el('span', { className: 'tag race', textContent: 'racing', title: 'Someone in this room is in the game' }));
      const who = el('div', { className: 'sub', textContent: r.players.map((p) => p.name).join(', ') });
      const join = el('button', { className: 'small', textContent: full ? 'Full' : 'Join', disabled: full || !!net.id });
      join.onclick = () => joinRoom(r.code, r.locked ? null : '', r.name);
      tb.append(el('tr', {},
        el('td', {}, title, who),
        el('td', { textContent: `${r.players.length}/${r.max}` }),
        el('td', { className: ping != null ? pingClass(ping) : '', textContent: ping != null ? `${ping} ms` : '–' }),
        el('td', {}, join)));
    }
  }

  // ---- in a room
  function drawRoom() {
    const inRoom = !!net.id;
    $('lobby').hidden = inRoom || gameStarted; $('inroom').hidden = !inRoom || gameStarted;
    if (!inRoom) return;
    $('room-title').textContent = net.info?.name || net.room;
    if (net.info?.locked) $('room-title').append(el('span', { className: 'tag', textContent: 'password' }));
    const ul = $('members'); ul.textContent = '';
    ul.append(el('li', {}, el('span', { textContent: `${myName() || 'Player'} (you)` }), el('span', { className: 'muted', textContent: net.relayRtt != null ? `server ${Math.round(net.relayRtt)} ms` : '' })));
    for (const p of net.peers.values()) {
      const r = net.rttTo(p);
      const right = el('span', {},
        el('span', { className: r != null ? pingClass(r) : 'muted', textContent: r != null ? `${Math.round(r)} ms` : '…' }),
        el('span', { className: 'muted', textContent: ` · ${p.direct ? 'direct' : 'via server'}${p.state === 'game' ? ' · in game' : ''}` }));
      ul.append(el('li', {}, el('span', { textContent: p.name }), right));
    }
    if (!net.peers.size) ul.append(el('li', {}, el('span', { className: 'muted', textContent: 'Waiting for friends… send them the invite link.' })));
    const best = net.bestHost();
    $('best-host').textContent = best ? `Best host: ${best.me ? 'you' : best.name} (slowest link ${best.worst} ms, counting ping and frame rate). The host has no input lag; everyone else has one round trip to the host.` : '';
    $('share-link').textContent = roomLink(net.room);
  }
  const redraw = () => { drawRooms(); drawRoom(); };

  async function joinRoom(code, password, label) {
    if (needName()) return;
    if (password == null) { askPassword(code, label); return; }
    message('');
    try {
      await net.join(code, myName(), password);
      savePass(code, password);
      track('room_join', { locked: !!password });
      history.replaceState(null, '', '#room=' + code);
      $('pass-form').hidden = true; joining = null;
    } catch (e) {
      if (e.code === 'password') { askPassword(code, label, e.message); return; }
      message(e.message);
      if (e.code === 'gone') history.replaceState(null, '', location.pathname + location.search);
    }
    redraw();
  }
  function askPassword(code, label, err) {
    joining = code;
    $('pass-label').textContent = `Password for ${label || code}`;
    $('pass-form').hidden = false; $('create-form').hidden = true;
    if (err) message(err);
    $('j-pass').value = ''; $('j-pass').focus();
  }
  $('pass-form').addEventListener('submit', (e) => { e.preventDefault(); if (joining) joinRoom(joining, $('j-pass').value); });
  $('j-cancel').onclick = () => { $('pass-form').hidden = true; joining = null; message(''); };

  $('show-create').onclick = () => {
    if (needName()) return;
    $('create-form').hidden = false; $('pass-form').hidden = true;
    if (!$('c-name').value) $('c-name').value = `${myName()}'s room`;
    $('c-name').focus();
  };
  $('c-cancel').onclick = () => { $('create-form').hidden = true; };
  $('create-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    if (needName()) return;
    message('');
    const pw = $('c-pass').value;
    try {
      const w = await net.create({ name: $('c-name').value.trim(), password: pw, max: Number($('c-max').value), listed: $('c-listed').checked }, myName());
      savePass(w.room, pw);
      track('room_create', { max: Number($('c-max').value), locked: !!pw, listed: $('c-listed').checked });
      history.replaceState(null, '', '#room=' + w.room);
      $('create-form').hidden = true;
    } catch (err) { message(err.message); }
    redraw();
  });
  $('leave-room').onclick = () => { net.leave(); history.replaceState(null, '', location.pathname + location.search); redraw(); };
  $('copy-link').onclick = () => navigator.clipboard?.writeText(roomLink(net.room)).then(() => {
    $('copy-link').textContent = 'Copied'; setTimeout(() => { $('copy-link').textContent = 'Copy invite link'; }, 1200);
  });

  net.addEventListener('rooms', redraw);
  net.addEventListener('status', drawRoom);
  net.addEventListener('log', (e) => log('online:', e.detail));
  net.addEventListener('disconnected', () => { server('offline', 'bad'); redraw(); if (!gameStarted) setTimeout(connect, 3000); });

  // invite links (#room=code) join straight away (asking for the password if needed)
  async function connect() {
    server('connecting…');
    try { await net.connect(); } catch (e) { server('offline', 'bad'); setTimeout(connect, 3000); return; }
    server('online', 'ok');
    const code = roomFromHash();
    if (code && !net.id) {
      if (myName()) joinRoom(code, savedPass(code), code);
      else { message('Enter your name, then join the room from the invite link.'); name.focus();
        name.addEventListener('change', () => { if (!net.id && roomFromHash() === code) joinRoom(code, savedPass(code), code); }, { once: true }); }
    }
    redraw();
  }
  window.addEventListener('hashchange', () => { const c = roomFromHash(); if (c && c !== net.room && !net.id) joinRoom(c, savedPass(c), c); });
  redraw();
  connect();
}

// ---------------------------------------------------------------- activity
// Start page: who is around (/__live on serve.mjs), the most active racers, the daily race,
// push notifications, and a note when the server has a newer build than this page.
function setupActivity() {
  let topWin = '7d', last = null;
  const fmtIn = (ms) => { const m = Math.round(ms / 60e3); return m < 60 ? `${m} min` : `${Math.floor(m / 60)} h ${m % 60} min`; };
  function draw(d) {
    last = d;
    $('live-line').textContent = d.racing + d.lobby === 0 ? 'Nobody online right now. Open a room and share the link!'
      : `${d.racing} racing · ${d.lobby} in the lobby · ${d.rooms} open room${d.rooms === 1 ? '' : 's'}`;
    $('live-line').classList.toggle('on', d.racing + d.lobby > 0);
    $('live-names').replaceChildren(...d.names.map((p) => el('span', { className: 'ntag' + (p.racing ? ' racing' : ''), textContent: p.n, title: p.racing ? 'racing' : 'in the lobby' })));
    const rows = d.top?.[topWin] || [];
    $('top').hidden = !['24h', '7d', 'all'].some((k) => d.top?.[k]?.length);
    $('top-list').replaceChildren(...(rows.length ? rows.map((r, i) => el('li', {}, el('span', { className: 'r', textContent: i + 1 }), el('span', { className: 'n', textContent: r.n }), el('span', { className: 'f', textContent: r.min >= 60 ? `${Math.floor(r.min / 60)} h ${r.min % 60} min` : `${r.min} min` })))
      : [el('li', { className: 'muted', textContent: 'No races in this time yet.' })]));
    if (d.daily) {
      const until = d.daily.next - Date.now();
      $('daily').hidden = false;
      $('daily').replaceChildren(el('b', { textContent: 'Daily race' }), ` every day at ${new Date(d.daily.next).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })} · `,
        el('span', { className: until <= 0 ? 'live' : '', textContent: until <= 0 ? 'on now, join a room!' : `starts in ${fmtIn(until)}` }));
    } else $('daily').hidden = true;
    if (d.build && buildInfo?.built && d.build !== buildInfo.built) {
      if ($('update').hidden) log(`a newer build is on the server (${d.build}, this page has ${buildInfo.built})`);
      $('update').hidden = gameStarted;
    }
  }
  $('top-tabs').onclick = (e) => {
    const w = e.target.dataset?.w; if (!w) return;
    topWin = w; for (const b of $('top-tabs').children) b.classList.toggle('on', b === e.target);
    if (last) draw(last);
  };
  $('update-reload').onclick = () => location.reload();
  const poll = () => fetch('__live', { cache: 'no-store' }).then((r) => r.ok && r.json()).then((d) => d && draw(d)).catch(() => {});
  poll(); setInterval(() => { if (!gameStarted || Math.random() < 0.25) poll(); }, 15000);
  setupNotify();
}

// "Notify me": a push when someone opens a room, and for the daily race (web/push.mjs, sw.js)
function setupNotify() {
  const b = $('notify');
  if (!('serviceWorker' in navigator) || !('PushManager' in window) || !('Notification' in window)) { b.hidden = true; return; }
  let sub = null;
  const draw = () => {
    b.hidden = false;
    b.disabled = Notification.permission === 'denied';
    b.textContent = Notification.permission === 'denied' ? 'Notifications are blocked in this browser' : sub ? '🔔 Notifications on (click to turn off)' : '🔔 Notify me when people race';
    b.classList.toggle('on', !!sub);
  };
  const reg = navigator.serviceWorker.register('sw.js').catch(() => null);
  reg.then((r) => r?.pushManager.getSubscription()).then((s) => { sub = s || null; draw(); });
  b.onclick = async () => {
    const r = await reg; if (!r) return;
    try {
      if (sub) {
        await fetch('__push/unsubscribe', { method: 'POST', body: JSON.stringify({ endpoint: sub.endpoint }) });
        await sub.unsubscribe(); sub = null; track('push', { on: false });
      } else {
        const { key } = await (await fetch('__push/key')).json();
        const raw = Uint8Array.from(atob(key.replace(/-/g, '+').replace(/_/g, '/')), (c) => c.charCodeAt(0));
        sub = await r.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: raw });
        const res = await fetch('__push/subscribe', { method: 'POST', body: JSON.stringify(sub.toJSON()) });
        if (!res.ok) throw new Error('the server did not take it');
        track('push', { on: true });
      }
    } catch (e) { log('notifications:', e.message); }
    draw();
  };
}

const TIPS = [
  'Cars take damage in High Stakes. Keep it clean, or the repairs eat your winnings.',
  'In Hot Pursuit you can drive the police car. Call in roadblocks and spike strips.',
  'High Stakes race: the loser hands over the pink slip. Online, too.',
  'Manual transmission is faster once you get the hang of it.',
  'Online: whoever has the lowest ping should create the game. The host never lags.',
  'Two racers? Pick Peer-to-Peer in the host dialog so both get the same responsiveness.',
  'Click the picture to capture the mouse. Esc gives it back.',
  'Career mode: win tournaments for cash, then buy upgrades in the garage.',
];
function setupLogToggle() {
  const t = $('log-toggle');
  const draw = () => { $('log').hidden = !logWanted(); t.textContent = logWanted() ? 'Hide technical log' : 'Show technical log'; };
  t.onclick = (e) => { e.preventDefault(); try { localStorage.setItem('nfs4.log', logWanted() ? '0' : '1'); } catch {} draw(); };
  draw();
}
// ------------------------------------------------------------------- what's new
// The changelog (web/changelog.json: the changes, day by day, written for players; nfs3-webgl's
// tools/discord/changelog.mjs posts the same days to Discord) in a dialog, opened from the
// menu's What's New… or with #changelog in the address. The button says NEW until the newest
// day has been looked at; days the visitor hasn't seen yet are marked too.
function setupChangelog(prefix) {
  const dlg = $('changelog'), btn = $('changelog-open'), list = $('changelog-list');
  if (!dlg || !btn || !list) return;
  const KIND = { new: 'New', improved: 'Better', fixed: 'Fixed' };
  let data = null, seen = '';
  try { seen = localStorage.getItem(prefix + 'changelogSeen') || ''; } catch {}
  const load = () => data ? Promise.resolve(data)
    : fetch('changelog.json?t=' + Date.now(), { cache: 'no-store' }).then((r) => { if (!r.ok) throw new Error(r.status); return r.json(); }).then((j) => (data = j));
  const dateText = (d) => { try { return new Date(d + 'T12:00:00Z').toLocaleDateString('en-GB', { weekday: 'short', day: 'numeric', month: 'long', year: 'numeric', timeZone: 'UTC' }); } catch { return d; } };
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
  function draw(before) {
    list.replaceChildren(...data.days.map((day) => {
      const sec = el('section', before && day.date > before ? 'unseen' : '');
      const h = el('h3', null, dateText(day.date));
      if (before && day.date > before) h.append(el('i', null, 'new'));
      const ul = el('ul');
      for (const [kind, text] of day.items) { const li = el('li'); li.append(el('span', 'ck ' + kind, KIND[kind] || kind), el('span', null, text)); ul.append(li); }
      sec.append(h, ul);
      return sec;
    }));
    list.scrollTop = 0;
  }
  async function open() {
    const before = seen;
    if (!dlg.open) { try { dlg.showModal(); } catch { dlg.setAttribute('open', ''); } }
    list.textContent = 'Loading…';
    try { await load(); draw(before); } catch { list.textContent = 'The list of changes couldn\'t be loaded. Try again in a moment.'; return; }
    const newest = data.days[0]?.date || '';
    if (newest > seen) { seen = newest; try { localStorage.setItem(prefix + 'changelogSeen', seen); } catch {} }
    btn.classList.remove('fresh');
  }
  btn.addEventListener('click', (e) => { e.preventDefault(); open(); });
  $('changelog-close')?.addEventListener('click', () => dlg.close());
  dlg.addEventListener('click', (e) => { if (e.target === dlg) dlg.close(); });     // a click on the backdrop
  dlg.addEventListener('close', () => { if (location.hash === '#changelog') history.replaceState(null, '', location.pathname + location.search); });
  const fromHash = () => { if (location.hash === '#changelog') open(); };
  window.addEventListener('hashchange', fromHash);
  fromHash();
  load().then((d) => { if ((d.days[0]?.date || '') > seen) btn.classList.add('fresh'); }).catch(() => {});
}

async function init() {
  setupLogToggle();
  $('tip').textContent = TIPS[Math.floor(Math.random() * TIPS.length)];
  setupOnline();
  setupActivity();
  setupChangelog('nfs4.');
  log('page loaded; crossOriginIsolated =', crossOriginIsolated, '; SharedArrayBuffer =', typeof SharedArrayBuffer);
  if (!crossOriginIsolated) {
    status('This page must be served with cross-origin isolation (COOP/COEP headers). Use web/serve.mjs.', true);
    return;
  }
  if (!navigator.storage?.getDirectory) { status('This browser has no Origin Private File System support.', true); return; }
  try { buildInfo = await (await fetch('dist/build-info.json', { cache: 'no-cache' })).json(); } catch {
    status('No recompiled build found in web/dist. Run ./build-web.sh first.', true); return;
  }
  // rooms only take players of the same build (different builds desync in a race)
  net.build = `${buildInfo.built || ''}:${(buildInfo.exe_sha256 || '').slice(0, 8)}`;
  // network worker (net-worker.js): relayed packets bypass the main thread. Off by default until
  // it has been tried in real races: ?networker=1 turns it on (remembered), ?networker=0 off.
  try {
    const q = new URLSearchParams(location.search).get('networker');
    if (q != null) localStorage.setItem('nfs4.networker', q === '1' ? '1' : '0');
    net.useWorker = localStorage.getItem('nfs4.networker') === '1';
  } catch {}
  log('build', buildInfo.built || '?');
  const refresh = async () => {
    const ok = await hasGame();
    $('pick-label').hidden = ok; $('start').hidden = !ok; $('forget').hidden = !ok;
    status(ok ? 'Game files are stored in this browser. Ready.' : 'Choose your NFS4 folder: NFS: High Stakes with the Modern Patch (beta) installed, the one with nfs4.exe, data and savedata.');
  };
  $('pick').addEventListener('change', async (e) => {
    try { await importFolder(e.target.files); } catch (err) { status(err.message, true); $('progress').hidden = true; return; }
    track('import', { files: e.target.files.length });
    await refresh();
  });
  $('forget').addEventListener('click', async () => {
    const root = await navigator.storage.getDirectory();
    await root.removeEntry(GAME_DIR, { recursive: true }).catch(() => {});
    await refresh();
  });
  // a server started with --game offers its game folder: import it automatically
  // ?devimport also forces the attempt and reports errors
  const forced = new URLSearchParams(location.search).has('devimport');
  await checkPack();
  if (await hasGame()) mark('files');
  const pack = await hasGame() ? null : await packSource();
  if (pack) {
    const d0 = performance.now();
    try { await importPack(pack); track('import', { from: 'pack', secs: (performance.now() - d0) / 1000 }); } catch (err) { status(err.message, true); $('progress').hidden = true; log('game pack:', err.message); track('error', { kind: 'download', msg: err.message }); return; }
    phase.download = Math.round((performance.now() - d0) / 100) / 10; mark('files');
  } else if (!(await hasGame()) && (forced || await serverHasGame())) {
    status('Downloading the game files into this browser…');
    const d0 = performance.now();
    try { await importFolder(await serverFiles()); track('import', { from: 'server', secs: (performance.now() - d0) / 1000 }); } catch (err) { status(err.message, true); return; }
    phase.download = Math.round((performance.now() - d0) / 100) / 10; mark('files');
  }
  if (new URLSearchParams(location.search).has('autostart') && (await hasGame())) { start().catch((e) => { console.error('Failed to start:', e.stack || e); status('Failed to start: ' + e.message, true); $('setup-wrap').hidden = false; }); }
  $('start').addEventListener('click', () => { if (!earlyAudio) earlyAudio = makeAudioContext(); start().catch((e) => { console.error(e); status('Failed to start: ' + e.message, true); $('start').disabled = false; }); });
  await refresh();
}
init();
