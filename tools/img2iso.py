"""CloneCD/raw .img (2352-byte sectors, Mode 1 or Mode 2 Form 1) -> 2048-byte ISO data track.
usage: python -I tools/img2iso.py in.img out.iso"""
import sys
src, dst = sys.argv[1], sys.argv[2]
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"
with open(src, "rb") as f, open(dst, "wb") as o:
    n = 0
    while True:
        s = f.read(2352)
        if len(s) < 2352:
            break
        if s[:12] != SYNC:      # audio / end of data track
            break
        mode = s[15]
        o.write(s[16:2064] if mode == 1 else s[24:2072])
        n += 1
print(f"{n} sectors -> {dst}")
