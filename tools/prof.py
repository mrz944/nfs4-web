#!/usr/bin/env python3
"""Summarise an RT_PROF sample file: python3 tools/prof.py build/nfs4 prof.bin [top]
File = u64 &main, then u64 PCs. <file>.maps = /proc/self/maps at start (for shared libs)."""
import bisect, collections, os, struct, subprocess, sys
exe, samples = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
def symtab(path):
    out = []
    for line in subprocess.run(['nm', '-n', '--defined-only', path], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tTwWiI': out.append((int(p[0], 16), p[2]))
    if not out:
        for line in subprocess.run(['nm', '-D', '-n', '--defined-only', path], capture_output=True, text=True).stdout.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in 'tTwWiI': out.append((int(p[0], 16), p[2]))
    return out
syms = symtab(exe); addrs = [a for a, _ in syms]
data = open(samples, 'rb').read()
vals = struct.unpack(f'<{len(data)//8}Q', data[: len(data) // 8 * 8])
mainrt, pcs = vals[0], vals[1:]
bias = mainrt - dict((n, a) for a, n in syms)['main']
libs = []   # (lo, hi, offset, path)
if os.path.exists(samples + '.maps'):
    for line in open(samples + '.maps'):
        p = line.split()
        if len(p) >= 6 and 'x' in p[1] and p[5].startswith('/') and os.path.realpath(p[5]) != os.path.realpath(exe):
            lo, hi = (int(x, 16) for x in p[0].split('-')); libs.append((lo, hi, int(p[2], 16), p[5]))
libsyms = {}
def libname(pc):
    for lo, hi, off, path in libs:
        if lo <= pc < hi:
            if path not in libsyms: t = symtab(path); libsyms[path] = (t, [a for a, _ in t])
            t, a = libsyms[path]
            # file offset ~ vaddr for typical shared objects
            v = pc - lo + off; i = bisect.bisect_right(a, v) - 1
            return os.path.basename(path), (t[i][1] if i >= 0 else '?')
    return None, '[unknown]'
fn = collections.Counter(); mod = collections.Counter()
for pc in pcs:
    a = pc - bias; i = bisect.bisect_right(addrs, a) - 1
    if 0 <= i and a < addrs[-1] + 0x100000 and a >= addrs[0]:
        name = syms[i][1]
        if name.startswith('f_'):
            g = int(name[2:10], 16)
            m = 'softtria.dll' if 0xB00000 <= g < 0xC00000 else 'eacsnd.dll' if 0xC00000 <= g < 0xD00000 else 'nfs4.exe'
        else: m = 'runtime'
    else:
        m, name = libname(pc); m = m or '[unknown]'; name = f'{m}:{name}'
    fn[name] += 1; mod[m] += 1
n = len(pcs)
print(f'{n} samples')
for k, v in mod.most_common(12): print(f'  {100*v/n:5.1f}%  {k}')
print()
for k, v in fn.most_common(top): print(f'  {100*v/n:5.1f}%  {k}')
