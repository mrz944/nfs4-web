// Writing files into the browser's private storage (OPFS). Most browsers have
// FileSystemFileHandle.createWritable(); Safari before 26 (iPhones on iOS 18) doesn't, and there
// the bytes go to a small worker (opfs-writer.js) that writes them with a sync access handle.
// openWritable(fileHandle) gives { write(data), close() } either way: data is a string (UTF-8),
// bytes or a Blob.
let worker = null, seq = 0;
const waiting = new Map();

function call(msg, transfer = []) {
  if (!worker) {
    worker = new Worker(new URL('./opfs-writer.js', import.meta.url));
    worker.onmessage = ({ data: m }) => {
      const w = waiting.get(m.id);
      if (!w) return;
      waiting.delete(m.id);
      if (m.err) { const e = new Error(m.err); e.name = m.name || 'Error'; w.reject(e); } else w.resolve(m.ok);
    };
    worker.onerror = (e) => {
      for (const w of waiting.values()) w.reject(new Error('the file writer stopped: ' + (e.message || 'error')));
      waiting.clear(); worker = null;
    };
  }
  return new Promise((resolve, reject) => {
    const id = ++seq;
    waiting.set(id, { resolve, reject });
    worker.postMessage({ ...msg, id }, transfer);
  });
}

async function ownBytes(data) {                          // a copy the worker can take over
  if (typeof data === 'string') return new TextEncoder().encode(data);
  if (data instanceof Blob) return new Uint8Array(await data.arrayBuffer());
  if (data instanceof ArrayBuffer) return new Uint8Array(data.slice(0));
  return new Uint8Array(data.buffer, data.byteOffset, data.byteLength).slice();
}

export async function openWritable(fh) {
  if (typeof fh.createWritable === 'function') return fh.createWritable();
  const path = await (await navigator.storage.getDirectory()).resolve(fh);
  if (!path) throw new Error('the file is outside the browser storage');
  const h = await call({ op: 'open', path });
  const CHUNK = 8 << 20;
  return {
    async write(data) {
      if (data instanceof Blob && data.size > CHUNK) {
        for (let o = 0; o < data.size; o += CHUNK) await this.write(data.slice(o, o + CHUNK));
        return;
      }
      const u = await ownBytes(data);
      await call({ op: 'write', h, buf: u.buffer }, [u.buffer]);
    },
    async close() { await call({ op: 'close', h }); },
  };
}

// the whole file in one go
export async function writeFile(dir, name, data) {
  const w = await openWritable(await dir.getFileHandle(name, { create: true }));
  await w.write(data);
  await w.close();
}
