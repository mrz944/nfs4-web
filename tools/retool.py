"""Reverse-engineering helpers over a PE (exe or dll).

python -I tools/retool.py <pe> dis <addr> [n]        disassemble n insns
python -I tools/retool.py <pe> xref <addr>           insns referencing addr (imm or mem disp)
python -I tools/retool.py <pe> str <text>            VA of a string
python -I tools/retool.py <pe> export <name> [n]     disassemble an export
python -I tools/retool.py <pe> func <addr>           whole discovered function, blocks in order
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Image  # noqa: E402
from discover import Program  # noqa: E402
import capstone as cs  # noqa: E402

X = cs.x86


def load(path):
    img = Image(path)
    prog = Program(img)
    prog.run()
    return img, prog


def dis(img, a, n=40):
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
    for k, i in enumerate(md.disasm(img.bytes(a, n * 15), a)):
        if k >= n:
            break
        print(f"  {i.address:08x}  {i.mnemonic} {i.op_str}")


def xref(prog, target):
    out = []
    for ins in prog.insns.values():
        for o in ins.ops:
            if o.type == X.X86_OP_IMM and (o.imm & 0xFFFFFFFF) == target:
                out.append(ins)
            elif o.type == X.X86_OP_MEM and (o.mem.disp & 0xFFFFFFFF) == target:
                out.append(ins)
    for ins in sorted(out, key=lambda i: i.addr):
        f = owner(prog, ins.addr)
        print(f"  {ins.addr:08x}  [{f:08x}]  {ins.text}")
    return out


_owner = None


def owner(prog, a):
    global _owner
    if _owner is None:
        _owner = {}
        for f in prog.funcs.values():
            for b in f.blocks.values():
                for i in b:
                    _owner[i.addr] = max(_owner.get(i.addr, 0), f.start)
    return _owner.get(a, 0)


def func(prog, a):
    f = prog.funcs[a]
    for b in sorted(f.blocks):
        print(f" L_{b:08x}:")
        for i in f.blocks[b]:
            print(f"  {i.addr:08x}  {i.text}")


def find_str(img, text):
    b = text.encode() + b"\0"
    i = img.mem.find(b)
    res = []
    while i >= 0:
        res.append(img.base + i)
        i = img.mem.find(b, i + 1)
    return res


if __name__ == "__main__":
    path, cmd = sys.argv[1], sys.argv[2]
    if cmd == "dis":
        img = Image(path)
        dis(img, int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 40)
    elif cmd == "str":
        print([hex(x) for x in find_str(Image(path), sys.argv[3])])
    elif cmd == "export":
        img = Image(path)
        a = [k for k, v in img.exports.items() if v == sys.argv[3]][0]
        print(hex(a))
        dis(img, a, int(sys.argv[4]) if len(sys.argv) > 4 else 60)
    else:
        img, prog = load(path)
        if cmd == "xref":
            for t in sys.argv[3:]:
                print(f"xrefs to {t}:")
                xref(prog, int(t, 16))
        elif cmd == "func":
            func(prog, int(sys.argv[3], 16))
