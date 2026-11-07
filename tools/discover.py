"""Recursive-descent function discovery over a PE32 image.

Seeds: entry point, exports, relocated pointers that land in code (vtables,
callbacks, jump tables), and every direct call target found while decoding.
Output: functions -> basic blocks -> decoded instructions.
"""
import capstone as cs
from capstone import x86 as X

JCC = {
    "ja", "jae", "jb", "jbe", "je", "jne", "jg", "jge", "jl", "jle", "jo", "jno",
    "jp", "jnp", "js", "jns", "jcxz", "jecxz", "loop", "loope", "loopne",
}


class Insn:
    __slots__ = ("addr", "size", "mnem", "ops", "raw", "text")

    def __init__(self, i):
        self.addr, self.size, self.mnem = i.address, i.size, i.mnemonic
        self.ops = list(i.operands)
        self.raw = i
        self.text = f"{i.mnemonic} {i.op_str}".strip()


class Func:
    def __init__(self, start):
        self.start = start
        self.blocks = {}  # start va -> [Insn]
        self.calls = set()
        self.indirect_jumps = []  # unresolved jmp addrs
        self.jump_tables = {}  # jmp insn addr -> [targets]
        self.bad = None


class Program:
    def __init__(self, img):
        self.img = img
        self.md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
        self.md.detail = True
        self.insns = {}  # va -> Insn (global decode cache)
        self.funcs = {}
        self.reloc_set = set(img.relocs)

    def decode(self, va):
        if va in self.insns:
            return self.insns[va]
        if not self.img.is_code(va):
            return None
        for i in self.md.disasm(self.img.bytes(va, 16), va, 1):
            ins = Insn(i)
            self.insns[va] = ins
            return ins
        return None

    def jump_table(self, ins):
        """jmp dword ptr [reg*4 + table]: read consecutive relocated entries."""
        op = ins.ops[0]
        if op.type != X.X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0:
            return None
        tbl = op.mem.disp & 0xFFFFFFFF
        out = []
        a = tbl
        while a in self.reloc_set and self.img.is_code(self.img.u32(a)):
            out.append(self.img.u32(a))
            a += 4
        return out or None

    def explore(self, start):
        if start in self.funcs:
            return self.funcs[start]
        f = Func(start)
        self.funcs[start] = f
        work = [start]
        seen = set()
        while work:
            b = work.pop()
            if b in seen:
                continue
            seen.add(b)
            block, va = [], b
            while True:
                ins = self.decode(va)
                if ins is None:
                    f.bad = f"undecodable at {va:#x}"
                    break
                block.append(ins)
                m = ins.mnem
                nxt = va + ins.size
                op0 = ins.ops[0] if ins.ops else None
                if m == "call":
                    if op0.type == X.X86_OP_IMM:
                        f.calls.add(op0.imm & 0xFFFFFFFF)
                elif m == "jmp":
                    if op0.type == X.X86_OP_IMM:
                        t = op0.imm & 0xFFFFFFFF
                        work.append(t)
                    else:
                        jt = self.jump_table(ins)
                        if jt:
                            f.jump_tables[ins.addr] = jt
                            work.extend(jt)
                        else:
                            f.indirect_jumps.append(ins.addr)
                    break
                elif m in JCC:
                    work.append(op0.imm & 0xFFFFFFFF)
                    work.append(nxt)
                    break
                elif m in ("ret", "retf", "iret", "iretd", "hlt", "int3", "ud2"):
                    break
                va = nxt
                if va in seen or va in f.blocks:  # fallthrough into existing block
                    break
            f.blocks[b] = block
        return f

    def run(self, extra_seeds=()):
        img = self.img
        seeds = {img.entry, *img.exports, *extra_seeds}
        # Relocated pointers into code that are not inside an instruction stream
        # are likely function pointers (vtables/callbacks) or jump-table entries.
        # Includes relocs inside code: immediates like `push offset callback`.
        for r in img.relocs:
            t = img.u32(r)
            if img.is_code(t):
                seeds.add(t)
        pending = list(seeds)
        while pending:
            s = pending.pop()
            if s in self.funcs:
                continue
            f = self.explore(s)
            pending.extend(c for c in f.calls if c not in self.funcs and img.is_code(c))
        self.split_overlaps()
        return self.funcs

    def split_overlaps(self):
        """A jump table target or tail-jump may land inside another function's
        block; both views stay valid in C (each function owns its own copy)."""
        pass
