"""Report discovery coverage and instruction mix for a PE: python -I tools/analyze.py nfs4.exe"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Image  # noqa: E402
from discover import Program  # noqa: E402


def main(path):
    img = Image(path)
    prog = Program(img)
    funcs = prog.run()
    covered = sum(i.size for i in prog.insns.values())
    code_bytes = img.code_hi - img.code_lo
    bad = [f for f in funcs.values() if f.bad]
    ij = sum(len(f.indirect_jumps) for f in funcs.values())
    jt = sum(len(f.jump_tables) for f in funcs.values())
    print(f"functions: {len(funcs)}  blocks: {sum(len(f.blocks) for f in funcs.values())}")
    print(f"insns: {len(prog.insns)}  covered {covered}/{code_bytes} bytes ({100*covered/code_bytes:.1f}%)")
    print(f"jump tables: {jt}  unresolved indirect jmps: {ij}  bad funcs: {len(bad)}")
    for f in bad[:10]:
        print("  bad", hex(f.start), f.bad)
    mix = collections.Counter(i.mnem for i in prog.insns.values())
    print(f"distinct mnemonics: {len(mix)}")
    print(" ".join(f"{m}:{n}" for m, n in mix.most_common()))
    return prog


if __name__ == "__main__":
    main(sys.argv[1])
