// Headless browser smoke test for the web build (dev only).
//   node tools/webtest.mjs [url] [seconds] [outdir]
// Launches Playwright's Chromium with remote debugging, logs console output,
// saves a screenshot every 5 s, and can send keys: KEYS="12:Enter,20:Enter"
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readdirSync, rmSync } from 'node:fs';
import { homedir } from 'node:os';
const url = process.argv[2] || 'http://localhost:8080/?devimport&autostart';
const secs = Number(process.argv[3] || 60);
const out = process.argv[4] || 'build/webtest';
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
const pw = homedir() + '/.cache/ms-playwright';
const cdir = readdirSync(pw).filter((d) => /^chromium-\d+$/.test(d)).sort().at(-1);
const exe = process.env.CHROME || `${pw}/${cdir}/chrome-linux64/chrome`;
const port = 9333;
const chrome = spawn(exe, ['--headless=new', `--remote-debugging-port=${port}`, '--user-data-dir=' + out + '/profile',
  '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--autoplay-policy=no-user-gesture-required',
  '--no-first-run', '--window-size=1280,960'], { stdio: ['ignore', 'ignore', 'pipe'] });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let ws;
for (let i = 0; i < 50 && !ws; i++) {
  await sleep(200);
  try { ws = (await (await fetch(`http://127.0.0.1:${port}/json/version`)).json()).webSocketDebuggerUrl; } catch {}
}
if (!ws) { console.error('chrome did not start'); process.exit(1); }
const sock = new WebSocket(ws);
await new Promise((r) => sock.addEventListener('open', r));
let id = 0; const pending = new Map();
const send = (method, params = {}, sessionId) => new Promise((res) => {
  const i = ++id; pending.set(i, res); sock.send(JSON.stringify({ id: i, method, params, ...(sessionId ? { sessionId } : {}) }));
});
const t0 = Date.now(); const ts = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);
let page = null, pageTarget = null;
sock.addEventListener('message', (m) => {
  const msg = JSON.parse(m.data);
  if (process.env.CDP_DEBUG) console.log('<<', String(m.data).slice(0, 200));
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg.result ?? msg); pending.delete(msg.id); return; }
  const p = msg.params || {};
  if (msg.method === 'Runtime.consoleAPICalled') console.log(ts(), p.args.map((a) => a.value ?? a.description).join(' '));
  else if (msg.method === 'Runtime.exceptionThrown') console.log(ts(), 'EXCEPTION', p.exceptionDetails.exception?.description || p.exceptionDetails.text);
  else if (msg.method === 'Target.attachedToTarget') {
    const sid = p.sessionId;
    if (p.targetInfo.targetId === pageTarget) page = sid;
    else if (p.targetInfo.type !== 'worker' && p.targetInfo.type !== 'iframe') { send('Runtime.runIfWaitingForDebugger', {}, sid); return; }
    send('Runtime.enable', {}, sid);
    send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }, sid);
    send('Runtime.runIfWaitingForDebugger', {}, sid);
  }
});
pageTarget = (await send('Target.createTarget', { url: 'about:blank' })).targetId;
await send('Target.attachToTarget', { targetId: pageTarget, flatten: true });
while (!page) await sleep(50);
await send('Page.enable', {}, page);
await send('Page.navigate', { url }, page);
const keys = (process.env.KEYS || '').split(',').filter(Boolean).map((k) => { const [t, key] = k.split(':'); return { t: Number(t), key }; });
const CODES = { Enter: 13, Escape: 27, ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, Space: 32 };
for (let s = 1; s <= secs; s++) {
  await sleep(1000);
  for (const k of keys.filter((k) => k.t === s)) {
    const vk = CODES[k.key] ?? k.key.charCodeAt(0);
    for (const type of ['rawKeyDown', 'keyUp']) { await send('Input.dispatchKeyEvent', { type, code: k.key.length === 1 ? 'Key' + k.key : k.key, key: k.key, windowsVirtualKeyCode: vk }, page); await sleep(80); }
    console.log(ts(), 'sent key', k.key);
  }
  if (s % 5 === 0) {
    const r = await send('Page.captureScreenshot', { format: 'png' }, page);
    if (r.data) writeFileSync(`${out}/shot-${String(s).padStart(3, '0')}.png`, Buffer.from(r.data, 'base64'));
  }
}
chrome.kill();
process.exit(0);
