// Writes files into the browser's private storage (OPFS) for browsers without
// FileSystemFileHandle.createWritable() (Safari before 26, so iPhones on iOS 18): a sync access
// handle does the same job, and those only exist in a worker. Driven by opfs.js.
// (Older Safari returned promises from the sync handle's methods: every call is awaited.)
const files = new Map();
let next = 1;
onmessage = async ({ data: m }) => {
  try {
    let ok = 1;
    if (m.op === 'open') {
      let d = await navigator.storage.getDirectory();
      for (const p of m.path.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
      const fh = await d.getFileHandle(m.path.at(-1), { create: true });
      const h = await fh.createSyncAccessHandle();
      await h.truncate(0);
      ok = next++;
      files.set(ok, { h, at: 0 });
    } else if (m.op === 'write') {
      const f = files.get(m.h), u = new Uint8Array(m.buf);
      for (let off = 0; off < u.length;) {
        const n = await f.h.write(u.subarray(off), { at: f.at + off });
        if (!(n > 0)) throw new Error('could not write the file');
        off += n;
      }
      f.at += u.length;
    } else if (m.op === 'close') {
      const f = files.get(m.h);
      files.delete(m.h);
      if (f) { await f.h.flush(); await f.h.close(); }
    }
    postMessage({ id: m.id, ok });
  } catch (e) {
    postMessage({ id: m.id, err: e.message || String(e), name: e.name });
  }
};
