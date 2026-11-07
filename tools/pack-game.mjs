#!/usr/bin/env node
// Packs a game folder for static hosting (Cloudflare R2 or any CDN with CORS):
//   node tools/pack-game.mjs game [build/r2]
// Writes <out>/<id>/index.json plus chunk-NNN.gz files. Each chunk is a gzip of several
// whole game files back to back, about 32 MB before compression, so a broken download
// resumes per chunk. <id> is a content hash: a changed game folder gets a new folder, and
// every file can be cached forever. Upload the <id> folder as is, without a Content-Encoding
// header (the page unpacks the gzip itself), then point web/game-source.json at it.
import { createHash } from 'node:crypto';
import { createReadStream, mkdirSync, readdirSync, readFileSync, renameSync, statSync, writeFileSync, createWriteStream } from 'node:fs';
import { join, resolve } from 'node:path';
import { createGzip } from 'node:zlib';
import { pipeline } from 'node:stream/promises';
import { PassThrough } from 'node:stream';

const [src, outArg = 'build/r2'] = process.argv.slice(2);
if (!src) { console.error('usage: node tools/pack-game.mjs GAME_DIR [OUT_DIR]'); process.exit(1); }
const gameDir = resolve(src), out = resolve(outArg);
const CHUNK = 32 << 20;
const junk = (n) => n.startsWith('._') || n === '.DS_Store' || n === 'Thumbs.db';

function list(dir, pre = '') {
  return readdirSync(dir, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1)).flatMap((e) => {
    if (junk(e.name)) return [];
    const p = join(dir, e.name), st = statSync(p);
    return st.isDirectory() ? list(p, pre + e.name + '/') : st.isFile() ? [[pre + e.name, st.size]] : [];
  });
}
const files = list(gameDir);
// empty folders (fedata/save, where the game writes its saves) are kept too
const emptyDirs = (function walk(dir, pre = '') {
  const es = readdirSync(dir, { withFileTypes: true }).filter((e) => !junk(e.name));
  if (pre && !es.length) return [pre.slice(0, -1)];
  return es.filter((e) => statSync(join(dir, e.name)).isDirectory()).flatMap((e) => walk(join(dir, e.name), pre + e.name + '/'));
})(gameDir);
const exe = files.find(([p]) => p.toLowerCase() === 'nfs4.exe');
if (!exe) { console.error(`${gameDir} has no nfs4.exe`); process.exit(1); }

// group whole files into chunks of about CHUNK bytes
const chunks = [];
for (const [path, size] of files) {
  let c = chunks.at(-1);
  if (!c || (c.raw >= CHUNK)) chunks.push(c = { files: [], raw: 0 });
  c.files.push([path, size]); c.raw += size;
}

const tmp = join(out, '.tmp');
mkdirSync(tmp, { recursive: true });
const all = createHash('sha256');
let exeSha = '', done = 0;
const total = files.reduce((s, f) => s + f[1], 0);
for (let i = 0; i < chunks.length; i++) {
  const c = chunks[i], name = `chunk-${String(i).padStart(3, '0')}.gz`;
  const raw = new PassThrough(), gz = createGzip({ level: 9 }), dst = createWriteStream(join(tmp, name));
  const writing = pipeline(raw, gz, dst);
  for (const [path, size] of c.files) {
    const h = createHash('sha256');
    all.update(`${path}\0${size}\0`);
    for await (const b of createReadStream(join(gameDir, path))) {
      h.update(b); all.update(b);
      if (!raw.write(b)) await new Promise((r) => raw.once('drain', r));
    }
    if (path === exe[0]) exeSha = h.digest('hex');
    done += size;
  }
  raw.end(); await writing;
  c.name = name; c.gz = statSync(join(tmp, name)).size;
  process.stdout.write(`\r${name}: ${(done / 2 ** 20).toFixed(0)} / ${(total / 2 ** 20).toFixed(0)} MB`);
}
for (const d of emptyDirs) all.update(`dir\0${d}\0`);
const id = all.digest('hex').slice(0, 16);
const index = {
  format: 1, id, exe_sha256: exeSha, total, gz: chunks.reduce((s, c) => s + c.gz, 0), dirs: emptyDirs,
  chunks: chunks.map((c) => ({ name: c.name, raw: c.raw, gz: c.gz, files: c.files })),
};
writeFileSync(join(tmp, 'index.json'), JSON.stringify(index));
const dest = join(out, id);
try { statSync(dest); console.log(`\n${dest} already exists (same content), nothing new`); }
catch { renameSync(tmp, dest); console.log(`\n${files.length} files, ${(total / 2 ** 20).toFixed(0)} MB -> ${chunks.length} chunks, ${(index.gz / 2 ** 20).toFixed(0)} MB gzip`); }
console.log(`pack: ${dest}`);
console.log(`nfs4.exe sha256 ${exeSha}${(() => { try { const b = JSON.parse(readFileSync(resolve('web/dist/build-info.json'), 'utf8')); return b.exe_sha256 === exeSha ? ' (matches web/dist)' : ' (DOES NOT match web/dist/build-info.json)'; } catch { return ''; } })()}`);
