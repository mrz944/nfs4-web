// Local static server for the web build, with the headers shared-memory
// threads need (cross-origin isolation).
//   node web/serve.mjs [port] [--game DIR] [--daily HH:MM] [--tailscale [httpsPort]] [--funnel [httpsPort]] [--relay-tcp PORT|PATH]
// --daily HH:MM: a daily race at that time (server clock): shown on the start page, with push
// notifications 10 minutes before and at the start.
// --funnel publishes the page, WebAssembly build and configured game files publicly.
// Also serves the online lobby/relay (WebSocket /net, relay.mjs); --relay-tcp adds a
// raw-TCP endpoint on 127.0.0.1 for native test builds (RT_NET=127.0.0.1:PORT).
// --tailscale publishes the server on your tailnet over HTTPS (tailscale serve,
// tailnet only) and removes that again on exit. Plain http://<tailscale ip> is
// not enough: browsers allow shared-memory threads and OPFS only on https/localhost.
import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { execFileSync, spawn } from 'node:child_process';
import { appendFileSync, createReadStream, mkdirSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { handleUpgrade, listenRaw, liveSummary, onRoomCreated, roomStats, setPlayLog } from './relay.mjs';
import { createPush, dailyRace } from './push.mjs';
import { createAnalytics } from './analytics.mjs';

const root = resolve(fileURLToPath(new URL('.', import.meta.url)));
// --game DIR: serve game files for automatic browser download into OPFS.
// The HTTP listener stays on loopback; Tailscale provides public HTTPS.
const args = process.argv.slice(2);
const gi = args.indexOf('--game');
const gameDir = gi >= 0 ? resolve(args.splice(gi, 2)[1]) : null;
const ti = args.indexOf('--tailscale');
let tsPort = null;
if (ti >= 0) { tsPort = /^\d+$/.test(args[ti + 1] || '') ? Number(args.splice(ti, 2)[1]) : (args.splice(ti, 1), 10000); }
const fi = args.indexOf('--funnel');
const funnel = fi >= 0;
if (funnel) { tsPort = /^\d+$/.test(args[fi + 1] || '') ? Number(args.splice(fi, 2)[1]) : (args.splice(fi, 1), 443); }
const di = args.indexOf('--daily');
const dailyAt = di >= 0 ? args.splice(di, 2)[1] : '';
const ri = args.indexOf('--relay-tcp');
const rawArg = ri >= 0 ? args.splice(ri, 2)[1] : '';
const rawPort = /^\d+$/.test(rawArg) ? Number(rawArg) : rawArg;   // port or Unix socket path
const port = Number(args[0] || process.env.PORT || 8080);
function listFiles(dir, pre = '') {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? listFiles(join(dir, e.name), pre + e.name + '/') : e.isFile() ? [[pre + e.name, statSync(join(dir, e.name)).size]] : []);
}
const types = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm', '.json': 'application/json', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon', '.webmanifest': 'application/manifest+json', '.woff2': 'font/woff2', '.txt': 'text/plain; charset=utf-8',
};

// client logs: the page POSTs its log to /__log (see main.js); kept in build/clientlogs/
const logDir = resolve(root, '..', 'build', 'clientlogs');
const LOG_CAP = 20 << 20;
function clientLog(req, res) {
  const sid = (new URL(req.url, 'http://x').searchParams.get('s') || '').replace(/[^a-z0-9-]/gi, '').slice(0, 40);
  let body = '';
  req.setEncoding('utf8');
  req.on('data', (d) => { body += d; if (body.length > 1 << 20) req.destroy(); });
  req.on('end', () => {
    res.writeHead(204).end();
    if (!sid || !body) return;
    mkdirSync(logDir, { recursive: true });
    const f = join(logDir, sid + '.log');
    let size = 0; try { size = statSync(f).size; } catch { console.log(`[client] new session ${sid} (${req.socket.remoteAddress}) -> build/clientlogs/${sid}.log`); }
    if (size < LOG_CAP) appendFileSync(f, body);
    for (const l of body.split('\n')) if (/ERROR|ABORT|stall|stopped/.test(l)) console.log(`[client ${sid}] ${l.trim()}`);
  });
}

// room log: every 5 s, each room with 2+ players (names, state, ping to this server),
// next to the client logs as build/clientlogs/room-<code>.log, for looking into lag later
setInterval(() => {
  for (const r of roomStats()) {
    if (r.players.length < 2) continue;
    mkdirSync(logDir, { recursive: true });
    const line = `${new Date().toISOString()} ${r.racing ? 'racing' : 'lobby'} host=${r.host ?? '?'} ` +
      r.players.map((p) => `${p.name}:${p.state}:${p.rtt != null ? Math.round(p.rtt) + 'ms' : '?'}`).join(' ') + '\n';
    try { appendFileSync(join(logDir, `room-${r.code.replace(/[^a-z0-9]/gi, '')}.log`), line); } catch {}
  }
}, 5000).unref();

// Versioned script URLs. Cloudflare gives .js files its own browser cache time (4 h by default,
// whatever we send), so a browser could keep yesterday's nfs4.js and pair it with today's
// nfs4.wasm, which crashes at the first frame. So every script address the page uses carries
// ?v=<build>: index.html and our .js files are rewritten on the way out, and a new build means
// new addresses. Inside dist/nfs4.js the same goes for "nfs4.js" (its game threads load it
// again) and "nfs4.wasm". sw.js keeps a fixed address (a service worker is identified by it).
const SCRIPTS = ['main.js', 'net.js', 'net-worker.js', 'audio-worklet.js', 'dist/nfs4.js'];
let verCache = { t: 0, v: '' };
function version() {
  if (Date.now() - verCache.t < 2000) return verCache.v;
  const h = createHash('sha256');
  for (const f of [...SCRIPTS, 'index.html', 'dist/nfs4.wasm', 'dist/build-info.json']) { try { const st = statSync(join(root, f)); h.update(`${f}:${st.size}:${st.mtimeMs}`); } catch {} }
  verCache = { t: Date.now(), v: h.digest('hex').slice(0, 10) };
  return verCache.v;
}
function versioned(text) {
  const v = version();
  return text.replace(/(["'])(\.\/)?(main\.js|net\.js|net-worker\.js|audio-worklet\.js|dist\/nfs4\.js|nfs4\.js|nfs4\.wasm)\1/g, (m, q, dot, f) => `${q}${dot || ''}${f}?v=${v}${q}`);
}

// page analytics (analytics.mjs): events in build/analytics/, summary at /__stats
const analytics = createAnalytics(resolve(root, '..', 'build', 'analytics'));
const playDir = resolve(root, '..', 'build', 'analytics');
setPlayLog(playDir);

// push notifications (push.mjs) and the daily race
const push = createPush(resolve(root, '..', 'build', 'push'));
onRoomCreated((r) => push.roomOpened(r));
const daily = dailyAt ? dailyRace(dailyAt, (m) => push.notify(m)) : null;
if (dailyAt && !daily) { console.error('--daily needs HH:MM'); process.exit(1); }

// start page activity (/__live): who is online now, the most active racers (time spent in
// online games, from relay.mjs play-*.jsonl), the daily race and the current build
let topCache = { t: 0, v: null };
function topRacers() {
  if (Date.now() - topCache.t < 60e3) return topCache.v;
  const now = Date.now(), win = { '24h': 864e5, '7d': 7 * 864e5, all: Infinity }, sums = { '24h': new Map(), '7d': new Map(), all: new Map() };
  try {
    for (const f of readdirSync(playDir).filter((f) => f.startsWith('play-'))) {
      for (const line of readFileSync(join(playDir, f), 'utf8').split('\n')) {
        let r; try { r = JSON.parse(line); } catch { continue; }
        for (const k in win) if (now - r.t < win[k]) sums[k].set(r.n, (sums[k].get(r.n) || 0) + r.s);
      }
    }
  } catch {}
  const v = {};
  for (const k in sums) v[k] = [...sums[k]].sort((a, b) => b[1] - a[1]).slice(0, 10).map(([n, s]) => ({ n, min: Math.round(s / 60) }));
  topCache = { t: Date.now(), v };
  return v;
}
function live(res) {
  let build = '';
  try { build = JSON.parse(readFileSync(join(root, 'dist', 'build-info.json'), 'utf8')).built || ''; } catch {}
  res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Cross-Origin-Resource-Policy': 'same-origin' });
  res.end(JSON.stringify({ ...liveSummary(), top: topRacers(), daily: daily ? { next: daily.next(), time: daily.time } : null, build }));
}

const server = createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (req.method === 'POST' && url.pathname === '/__event') { analytics.handleEvent(req, res); return; }
  if (url.pathname === '/__stats') { analytics.handleStats(req, res, roomStats); return; }
  if (req.method === 'POST' && url.pathname === '/__log') { clientLog(req, res); return; }
  if (url.pathname === '/__live') { live(res); return; }
  if (url.pathname.startsWith('/__push/')) { push.handle(req, res, url.pathname.slice(1)); return; }
  let decoded;
  try { decoded = decodeURIComponent(url.pathname); } catch { res.writeHead(400).end('invalid path'); return; }
  let path = normalize(decoded).replace(/^([/\\])+/, '');
  if (path === '' || path.endsWith('/')) path += 'index.html';
  let base = root;
  if (gameDir && path === '__dev/game-list') {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cross-Origin-Resource-Policy': 'same-origin' });
    res.end(JSON.stringify(listFiles(gameDir))); return;
  }
  if (gameDir && path.startsWith('__dev/game/')) { base = gameDir; path = path.slice('__dev/game/'.length); }
  const file = join(base, path);
  if (!file.startsWith(base + sep)) { res.writeHead(403).end(); return; }
  let st;
  try { st = statSync(file); } catch { res.writeHead(404).end('not found'); return; }
  if (st.isDirectory()) { res.writeHead(404).end(); return; }
  // the page and our own scripts: script addresses get ?v=<build> (see versioned())
  const rewrite = base === root && (path === 'index.html' || SCRIPTS.includes(path));
  const body = rewrite ? Buffer.from(versioned(readFileSync(file, 'utf8'))) : null;
  res.writeHead(200, {
    'Content-Type': types[extname(file)] || 'application/octet-stream',
    'Content-Length': body ? body.length : st.size,
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'same-origin',
    'Cache-Control': 'no-cache',
  });
  if (req.method === 'HEAD') { res.end(); return; }
  if (rewrite) { res.end(body); return; }
  createReadStream(file).on('error', () => res.destroy()).pipe(res);
});
// online play: WebSocket lobby/relay at /net (see relay.mjs)
server.on('upgrade', (req, sock) => { if (new URL(req.url, 'http://x').pathname === '/net') handleUpgrade(req, sock); else sock.destroy(); });
if (rawPort) listenRaw(rawPort);
server.listen(port, '127.0.0.1', () => {
  console.log(`NFS4 web runtime: http://localhost:${port}/` + (gameDir ? `  (game folder: ${gameDir}, automatic download)` : ''));
  console.log(`stats:            http://localhost:${port}/__stats`);
  if (daily) console.log(`daily race:       ${daily.time} (server time), next ${new Date(daily.next()).toString().slice(0, 21)}; ${push.count} push subscriber(s)`);
  if (funnel && ![443, 8443, 10000].includes(tsPort)) { console.error('Funnel only works on ports 443, 8443 and 10000'); process.exit(1); }
  if (tsPort) startTailscale();
});

async function startTailscale() {
  const ts = (...a) => execFileSync('tailscale', a, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  try {
    // Activation can wait for account approval: keep serving and show the activation URL.
    await new Promise((resolve, reject) => {
      const child = spawn('tailscale', [funnel ? 'funnel' : 'serve', '--bg', `--https=${tsPort}`, `http://127.0.0.1:${port}`], { stdio: 'inherit' });
      child.once('error', reject);
      child.once('exit', (code) => code === 0 ? resolve() : reject(new Error(`Tailscale exited with code ${code}`)));
    });
    const name = JSON.parse(ts('status', '--json')).Self.DNSName.replace(/\.$/, '');
    const base = `https://${name}${tsPort === 443 ? '' : ':' + tsPort}/`;
    if (funnel) {
      console.log(`funnel:           ${base}  (public; game files and WebAssembly are served)`);
    } else console.log(`tailnet:          ${base}` + (gameDir ? '  (game files download automatically)' : ''));
  } catch (e) {
    console.error(`tailscale ${funnel ? 'funnel' : 'serve'} failed:`, (e.stderr || e.message).trim());
    if (funnel) console.error('(Funnel must be allowed for this machine in the tailnet policy: https://tailscale.com/kb/1223/funnel)');
    console.error('(if it asks for permission: sudo tailscale set --operator=$USER)');
    return;
  }
  const off = () => { try { ts(funnel ? 'funnel' : 'serve', `--https=${tsPort}`, 'off'); } catch {} process.exit(0); };
  process.on('SIGINT', off); process.on('SIGTERM', off);
}
