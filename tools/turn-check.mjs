// Checks the Cloudflare TURN key that serve.mjs uses for /__ice (web/serve.mjs iceServers):
// reads build/cloudflare/turn.json ({"id": "<TURN token ID>", "token": "<API token>"}), mints
// short-lived credentials the way the server does, and prints what the page would get, without
// the secrets. Run it after creating or changing the key:
//
//   node tools/turn-check.mjs
import { readFileSync, statSync, chmodSync } from 'node:fs';

const file = new URL('../build/cloudflare/turn.json', import.meta.url);
let key;
try { key = JSON.parse(readFileSync(file, 'utf8')); }
catch (e) { console.log(`No usable build/cloudflare/turn.json (${e.code || e.message}).`); process.exit(1); }
if (!key?.id || !key?.token) { console.log('turn.json needs both "id" (the TURN token ID) and "token" (its API token).'); process.exit(1); }
if (statSync(file).mode & 0o077) { chmodSync(file, 0o600); console.log('turn.json was readable by others: set to 600.'); }

const r = await fetch(`https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(key.id)}/credentials/generate-ice-servers`, {
  method: 'POST', headers: { Authorization: 'Bearer ' + key.token, 'Content-Type': 'application/json' }, body: JSON.stringify({ ttl: 600 }),
  signal: AbortSignal.timeout(10000),
});
const text = await r.text();
if (!r.ok) {
  console.log(`Cloudflare refused: HTTP ${r.status}.`);
  if (r.status === 401 || r.status === 403) console.log('The token does not match the ID, or it was revoked. Copy both again from the dashboard.');
  if (r.status === 404) console.log('No TURN key with that ID. "id" must be the TURN token ID, not the account ID.');
  process.exit(1);
}
const j = JSON.parse(text);
const servers = [].concat(j.iceServers || []);
const urls = servers.flatMap((s) => [].concat(s.urls));
const withCred = servers.filter((s) => s.username && s.credential).length;
console.log(`OK: ${urls.length} ICE URLs, ${withCred} with credentials.`);
for (const u of urls) console.log('  ' + u + (/:53\b/.test(u) ? '   (left out by the server: browsers block port 53)' : ''));
console.log('A server running the current web/serve.mjs hands fresh ones to the page at /__ice (a restart is needed once, to load that code).');
