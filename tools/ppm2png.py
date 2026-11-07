#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png [scale-down]  (binary P6, no dependencies)"""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
parts = d.split(maxsplit=4); w, h = int(parts[1]), int(parts[2]); px = parts[4]
k = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rows = b''.join(b'\0' + b''.join(px[(y*w + x)*3:(y*w + x)*3 + 3] for x in range(0, w, k)) for y in range(0, h, k))
ch = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w // k, h // k, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(rows)) + ch(b'IEND', b''))
