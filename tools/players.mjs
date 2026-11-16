// Players (web/players.mjs) from the terminal: look one up, and fix what a player can't.
//
//   node tools/players.mjs who <name|id>             names, sessions (with their log files), laps, online play
//   node tools/players.mjs list                      how many players, the newest ones
//   node tools/players.mjs give <old name> <name|id> the laps from before player keys under <old name> go to
//                                                    that player (a racer's earlier name, asked for in Discord)
//   node tools/players.mjs rename <name|id> <new>    no limits, even a name another player holds
//   node tools/players.mjs forget <name|id>          delete the player (its laps leave the boards)
//   --data DIR: the server's data folder (default: build/)
//
// Changes are appended to build/players/players.jsonl; a running serve.mjs reads them within
// a few seconds.
import { readdirSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createPlayers, foldName } from '../web/players.mjs';
import { createLaps } from '../web/laps.mjs';
import { playerReport } from '../web/player-stats.mjs';

const args = process.argv.slice(2);
const di = args.indexOf('--data');
const data = di >= 0 ? resolve(args.splice(di, 2)[1]) : resolve(fileURLToPath(new URL('../build', import.meta.url)));
const [cmd, a, b] = args;
const players = createPlayers(join(data, 'players'), { watch: false });
const laps = createLaps(join(data, 'laps'), players);
const ctx = { players, laps, playDir: join(data, 'analytics'), logDir: join(data, 'clientlogs'), sessionsDir: join(data, 'players') };
const when = (t) => (t ? new Date(t).toISOString().replace('T', ' ').slice(0, 16) : '-');
const find = (q) => { const pl = players.find(q); if (!pl) { console.error(`No player called ${q}.`); process.exit(1); } return pl; };
const events = () => {
  const out = [];
  let files = []; try { files = readdirSync(ctx.playDir).filter((f) => f.startsWith('events-')).sort().slice(-2); } catch {}
  for (const f of files) for (const l of readFileSync(join(ctx.playDir, f), 'utf8').split('\n')) { if (!l) continue; try { const r = JSON.parse(l); if (r.p) out.push(r); } catch {} }
  return out;
};

if (cmd === 'who' && a) {
  const pl = find(a), r = playerReport(ctx, pl, events());
  console.log(`${pl.n || '(no name)'}  id ${pl.p}  player since ${when(pl.t)}  last seen ${when(r.last)}`);
  console.log(`names: ${pl.hist.map((h) => `${h.n} (${when(h.t)})`).join(', ') || '-'}`);
  if (pl.legacy.size) console.log(`laps from before player keys under: ${[...pl.legacy].join(', ')}`);
  if (pl.merged.length) console.log(`joined from: ${pl.merged.map((m) => `${m.n || '?'} ${m.p} (${when(m.t)})`).join(', ')}`);
  console.log(`laps: ${r.laps}, online play: ${Math.round(r.play / 60)} min in ${r.playN} games, connection events: ${r.net.length}`);
  for (const x of r.best) console.log(`  ${x.track} ${x.dir}: ${x.time} (${x.car}, ${when(x.t)})`);
  console.log(`sessions (${r.sessions.length}, newest first):`);
  for (const s of r.sessions.slice(0, 30)) console.log(`  ${when(s.t)}  ${s.log}`);
  const errs = r.events.filter((e) => e.e === 'error').slice(-10);
  if (errs.length) { console.log('errors:'); for (const e of errs) console.log(`  ${when(e.t)} ${e.d.kind || ''} ${e.d.msg || ''} (${e.s})`); }
} else if (cmd === 'list') {
  const all = players.all().sort((x, y) => y.t - x.t);
  console.log(`${all.length} players, ${all.filter((pl) => pl.n).length} with a name`);
  for (const pl of all.slice(0, 25)) console.log(`  ${when(pl.t)}  ${pl.p}  ${pl.n || '(no name)'}  ${laps.lapsOf(pl.p).length} laps`);
} else if (cmd === 'give' && a && b) {
  const pl = find(b), f = foldName(a), had = players.legacyOwner(f);
  if (!players.legacyLaps(f)) { console.error(`No laps from before player keys under ${a}.`); process.exit(1); }
  players.write({ ev: 'legacy', p: pl.p, n: a, admin: true });
  console.log(`The laps under ${a} are ${pl.n}'s (${pl.p}) now${had && had !== pl.p ? `; they were ${players.nameOf(had)}'s (${had})` : ''}.`);
} else if (cmd === 'rename' && a && b) {
  const pl = find(a), was = pl.n, r = players.setName(pl, b, { admin: true });
  if (r.error) { console.error(`Not renamed: ${r.error}`); process.exit(1); }
  console.log(`${was} (${pl.p}) is ${r.n} now.`);
} else if (cmd === 'forget' && a) {
  const pl = find(a);
  players.write({ ev: 'forget', p: pl.p });
  console.log(`Deleted ${pl.n} (${pl.p}).`);
} else {
  console.log(readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n').slice(0, 12).join('\n'));
  process.exit(cmd ? 1 : 0);
}
