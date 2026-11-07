"""x86 -> C static recompiler.

usage: python -I tools/lift.py <exe or dll> <outdir> [--name N] [--base HEX] [--seeds FILE]

Each guest function becomes `void f_XXXXXXXX(CPU *c)`. Flags are function
locals (clang removes the dead ones); registers live in CPU. Unsupported
instructions become x86_trap() calls and are counted in the report.
"""
import re
import collections
import os
import sys

from capstone import x86 as X

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Image  # noqa: E402
from discover import Program, JCC  # noqa: E402

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
R8L = ["al", "cl", "dl", "bl"]
R8H = ["ah", "ch", "dh", "bh"]
SEG = ["es", "cs", "ss", "ds", "fs", "gs"]
MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
CT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
ST_ = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
BITS = {1: 8, 2: 16, 4: 32}

COND = {
    "ja": "(!cf && !zf)", "jae": "(!cf)", "jb": "(cf)", "jbe": "(cf || zf)",
    "je": "(zf)", "jne": "(!zf)", "jg": "(!zf && sf == of)", "jge": "(sf == of)",
    "jl": "(sf != of)", "jle": "(zf || sf != of)", "jo": "(of)", "jno": "(!of)",
    "jp": "(pf)", "jnp": "(!pf)", "js": "(sf)", "jns": "(!sf)",
}


MMX_OPS = {
    "paddb", "paddw", "paddd", "paddsb", "paddsw", "paddusb", "paddusw", "psubb", "psubw", "psubd",
    "psubsb", "psubsw", "psubusb", "psubusw", "pmullw", "pmulhw", "pmaddwd", "pcmpeqb", "pcmpeqw",
    "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd", "pand", "pandn", "por", "pxor", "psllw", "pslld",
    "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad", "punpcklbw", "punpcklwd", "punpckldq",
    "punpckhbw", "punpckhwd", "punpckhdq", "packsswb", "packuswb", "packssdw",
}


class Unsupported(Exception):
    pass


class Emitter:
    def __init__(self, prog):
        self.prog = prog
        self.img = prog.img
        self.unsupported = collections.Counter()
        self.func_set = set(prog.funcs)

    # ---------- operands ----------
    def rn(self, ins, r):
        return ins.raw.reg_name(r)

    def reg_read(self, name):
        if name in R32:
            return f"c->r[{R32.index(name)}]"
        if name in R16:
            return f"((uint16_t)c->r[{R16.index(name)}])"
        if name in R8L:
            return f"((uint8_t)c->r[{R8L.index(name)}])"
        if name in R8H:
            return f"((uint8_t)(c->r[{R8H.index(name)}] >> 8))"
        if name in SEG:
            return f"c->seg[{SEG.index(name)}]"
        raise Unsupported(f"reg {name}")

    def reg_write(self, name, v):
        if name in R32:
            return f"c->r[{R32.index(name)}] = (uint32_t)({v});"
        if name in R16:
            i = R16.index(name)
            return f"c->r[{i}] = (c->r[{i}] & 0xFFFF0000u) | ((uint32_t)({v}) & 0xFFFFu);"
        if name in R8L:
            i = R8L.index(name)
            return f"c->r[{i}] = (c->r[{i}] & 0xFFFFFF00u) | ((uint32_t)({v}) & 0xFFu);"
        if name in R8H:
            i = R8H.index(name)
            return f"c->r[{i}] = (c->r[{i}] & 0xFFFF00FFu) | (((uint32_t)({v}) & 0xFFu) << 8);"
        if name in SEG:
            return f"c->seg[{SEG.index(name)}] = (uint16_t)({v});"
        raise Unsupported(f"reg {name}")

    def addr(self, ins, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg_read(self.rn(ins, m.base)))
        if m.index:
            s = f" * {m.scale}" if m.scale != 1 else ""
            parts.append(f"{self.reg_read(self.rn(ins, m.index))}{s}")
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xFFFFFFFF:X}u")
        if m.segment and self.rn(ins, m.segment) == "fs":
            parts.append("c->fs_base")
        elif m.segment and self.rn(ins, m.segment) not in ("ds", "es", "ss", "cs"):
            raise Unsupported("segment " + self.rn(ins, m.segment))
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def rd(self, ins, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_read(self.rn(ins, op.reg))
        if op.type == X.X86_OP_IMM:
            return f"0x{op.imm & int(MASK[size][:-1], 16):X}u"
        if op.type == X.X86_OP_MEM:
            return f"RD{BITS[size]}({self.addr(ins, op)})"
        raise Unsupported("operand")

    def wr(self, ins, op, v, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_write(self.rn(ins, op.reg), v)
        if op.type == X.X86_OP_MEM:
            return f"WR{BITS[size]}({self.addr(ins, op)}, ({CT[size]})({v}));"
        raise Unsupported("write operand")

    # ---------- flags ----------
    def szp(self, r, size):
        b = BITS[size]
        return f"zf = (({r}) & {MASK[size]}) == 0; sf = (({r}) >> {b-1}) & 1; pf = parity8({r});"

    def arith(self, ins, kind):
        d, s = ins.ops[0], ins.ops[1]
        sz = d.size
        b = BITS[sz]
        a = self.rd(ins, d)
        bb = self.rd(ins, s, sz)
        if s.type == X.X86_OP_IMM and s.size < sz:  # sign-extended imm8
            bb = f"0x{s.imm & int(MASK[sz][:-1], 16):X}u"
        L = [f"uint32_t a = {a}, b = {bb}, r;"]
        if kind in ("add", "adc"):
            cin = " + cf" if kind == "adc" else ""
            L.append(f"uint64_t t = (uint64_t)a + b{cin}; r = (uint32_t)t & {MASK[sz]};")
            L.append(f"cf = (t >> {b}) & 1; of = (((a ^ r) & (b ^ r)) >> {b-1}) & 1;")
        elif kind in ("sub", "sbb", "cmp"):
            bin_ = " - cf" if kind == "sbb" else ""
            L.append(f"uint64_t t = (uint64_t)a - b{bin_}; r = (uint32_t)t & {MASK[sz]};")
            L.append(f"cf = (t >> {b}) & 1; of = (((a ^ b) & (a ^ r)) >> {b-1}) & 1;")
        elif kind in ("and", "test"):
            L.append("r = a & b; cf = 0; of = 0;")
        elif kind == "or":
            L.append("r = a | b; cf = 0; of = 0;")
        elif kind == "xor":
            L.append("r = a ^ b; cf = 0; of = 0;")
        L.append(self.szp("r", sz))
        if kind not in ("cmp", "test"):
            if kind == "xor" and d.type == s.type == X.X86_OP_REG and d.reg == s.reg:
                pass
            L.append(self.wr(ins, d, "r"))
        return ["{ " + " ".join(L) + " }"]

    def shift(self, ins, kind):
        d = ins.ops[0]
        sz = d.size
        b = BITS[sz]
        cnt = "1u" if len(ins.ops) == 1 else self.rd(ins, ins.ops[1], 1)
        L = [f"uint32_t n = ({cnt}) & 31; if (n) {{ uint32_t a = {self.rd(ins, d)}, r;"]
        if kind == "shl":
            L.append(f"r = (uint32_t)((uint64_t)a << n) & {MASK[sz]}; cf = (uint32_t)(((uint64_t)a << n) >> {b}) & 1;")
            L.append(f"of = ((r >> {b-1}) & 1) ^ cf;")
        elif kind == "shr":
            L.append(f"r = (a >> n); cf = (a >> (n - 1)) & 1; of = (a >> {b-1}) & 1;")
        elif kind == "sar":
            L.append(f"int32_t sa = ({ST_[sz]})a; r = (uint32_t)(sa >> (n > {b-1} ? {b-1} : n)) & {MASK[sz]};")
            L.append(f"cf = (uint32_t)(sa >> (n - 1 > {b-1} ? {b-1} : n - 1)) & 1; of = 0;")
        elif kind in ("rol", "ror"):
            L.append(f"uint32_t k = n % {b};")
            if kind == "rol":
                L.append(f"r = k ? ((a << k) | (a >> ({b} - k))) & {MASK[sz]} : a; cf = r & 1;")
                L.append(f"of = ((r >> {b-1}) ^ cf) & 1;")
            else:
                L.append(f"r = k ? ((a >> k) | (a << ({b} - k))) & {MASK[sz]} : a; cf = (r >> {b-1}) & 1;")
                L.append(f"of = ((r >> {b-1}) ^ (r >> {b-2})) & 1;")
            L.append(self.wr(ins, d, "r") + " }")
            return L
        elif kind in ("rcl", "rcr"):
            L.append(f"uint64_t w = ((uint64_t)cf << {b}) | a; uint32_t k = n % {b+1};")
            if kind == "rcl":
                L.append(f"w = (w << k) | (w >> ({b+1} - k));")
            else:
                L.append(f"w = (w >> k) | (w << ({b+1} - k));")
            L.append(f"r = (uint32_t)w & {MASK[sz]}; cf = (w >> {b}) & 1; of = 0;")
            L.append(self.wr(ins, d, "r") + " }")
            return L
        L.append(self.szp("r", sz))
        L.append(self.wr(ins, d, "r") + " }")
        return L

    # ---------- string ops ----------
    def strop(self, ins):
        m = ins.mnem.split(" ")[-1]
        # Take rep from the prefix bytes: Capstone drops F2 from `repne movsd`
        # (it collides with the SSE movsd encoding), but the CPU still repeats.
        rep = None
        for b in ins.raw.bytes:
            if b in (0xF2, 0xF3):
                rep = "repe" if b == 0xF3 else "repne"
            elif b not in (0x66, 0x67, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0xF0):
                break
        if rep and m[:4] in ("movs", "stos", "lods"):
            rep = "rep"
        base, sfx = m[:-1], m[-1]
        sz = {"b": 1, "w": 2, "d": 4}[sfx]
        bits = BITS[sz]
        st = f"(c->df ? -{sz} : {sz})"
        acc = {1: "al", 2: "ax", 4: "eax"}[sz]
        if base == "movs":
            body = f"WR{bits}(c->r[EDI], RD{bits}(c->r[ESI])); c->r[ESI] += {st}; c->r[EDI] += {st};"
        elif base == "stos":
            body = f"WR{bits}(c->r[EDI], ({CT[sz]}){self.reg_read(acc)}); c->r[EDI] += {st};"
        elif base == "lods":
            body = self.reg_write(acc, f"RD{bits}(c->r[ESI])") + f" c->r[ESI] += {st};"
        elif base in ("scas", "cmps"):
            if base == "scas":
                a, b = self.reg_read(acc), f"RD{bits}(c->r[EDI])"
                adv = f"c->r[EDI] += {st};"
            else:
                a, b = f"RD{bits}(c->r[ESI])", f"RD{bits}(c->r[EDI])"
                adv = f"c->r[ESI] += {st}; c->r[EDI] += {st};"
            body = (f"{{ uint32_t a = {a}, b = {b}; uint64_t t = (uint64_t)a - b; uint32_t r = (uint32_t)t & {MASK[sz]};"
                    f" cf = (t >> {bits}) & 1; of = (((a ^ b) & (a ^ r)) >> {bits-1}) & 1; {self.szp('r', sz)} }} {adv}")

        else:
            raise Unsupported(ins.mnem)
        if not rep:
            return [body]
        if rep == "rep" or base in ("movs", "stos", "lods"):
            return [f"while (c->r[ECX]) {{ {body} c->r[ECX]--; }}"]
        brk = "if (!zf) break;" if rep == "repe" else "if (zf) break;"
        return [f"while (c->r[ECX]) {{ {body} c->r[ECX]--; {brk} }}"]

    # ---------- x87 ----------
    def fpu(self, ins):
        m, ops = ins.mnem, ins.ops

        def stidx(op):
            n = self.rn(ins, op.reg)
            return int(n[3]) if n.startswith("st(") else None

        def fmem_load(op, integer):
            a = self.addr(ins, op)
            if integer:
                return {2: f"(double)(int16_t)RD16({a})", 4: f"(double)(int32_t)RD32({a})",
                        8: f"(double)(int64_t)RD64({a})"}[op.size]
            return {4: f"(double)RDF32({a})", 8: f"RDF64({a})", 10: f"RDF80({a})"}[op.size]

        ARITH = {"fadd": "{d} + {s}", "fmul": "{d} * {s}", "fsub": "{d} - {s}", "fsubr": "{s} - {d}",
                 "fdiv": "{d} / {s}", "fdivr": "{s} / {d}"}
        base = m[:-1] if m.endswith("p") and m[:-1] in ARITH else m
        ibase = "f" + m[2:] if m.startswith("fi") and ("f" + m[2:]) in ARITH else None
        if base in ARITH:
            pop = m.endswith("p")
            if not ops:  # faddp with no operands = st(1) op st0, pop
                d, s = "ST(1)", "ST(0)"
            elif ops[0].type == X.X86_OP_MEM:
                d, s = "ST(0)", fmem_load(ops[0], False)
            elif len(ops) == 2:
                d, s = f"ST({stidx(ops[0])})", f"ST({stidx(ops[1])})"
            elif pop:
                d, s = f"ST({stidx(ops[0])})", "ST(0)"
            else:
                d, s = "ST(0)", f"ST({stidx(ops[0])})"
            L = [f"{d} = {ARITH[base].format(d=d, s=s)};"]
            if pop:
                L.append("fpop(c);")
            return L
        if ibase:
            d, s = "ST(0)", fmem_load(ops[0], True)
            return [f"{d} = {ARITH[ibase].format(d=d, s=s)};"]
        if m in ("fld", "fild"):
            if ops[0].type == X.X86_OP_MEM:
                if m == "fild" and ops[0].size == 8:
                    return [f"fild64(c, (int64_t)RD64({self.addr(ins, ops[0])}));"]
                return [f"fpush(c, {fmem_load(ops[0], m == 'fild')});"]
            return [f"fpush_st(c, {stidx(ops[0])});"]
        if m in ("fst", "fstp"):
            L = []
            o = ops[0]
            if o.type == X.X86_OP_MEM:
                a = self.addr(ins, o)
                L.append({4: f"WRF32({a}, (float)ST(0));", 8: f"WRF64({a}, ST(0));", 10: f"WRF80({a}, ST(0));"}[o.size])
            else:
                L.append(f"fcopy(c, {stidx(o)}, 0);")
            if m == "fstp":
                L.append("fpop(c);")
            return L
        if m in ("fist", "fistp"):
            a = self.addr(ins, ops[0])
            t = {2: "int16_t", 4: "int32_t", 8: "int64_t"}[ops[0].size]
            w = {2: "WR16", 4: "WR32", 8: "WR64"}[ops[0].size]
            if ops[0].size == 8:
                L = [f"WR64({a}, (uint64_t)fist64(c));"]
            else:
                L = [f"{w}({a}, ({'uint' + t[3:]})({t})fround(c, ST(0)));"]
            if m == "fistp":
                L.append("fpop(c);")
            return L
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ficom", "ficomp"):
            if m.endswith("pp"):
                s = "ST(1)"
            elif not ops:
                s = "ST(1)"
            elif ops[0].type == X.X86_OP_MEM:
                s = fmem_load(ops[0], m.startswith("fi"))
            else:
                s = f"ST({stidx(ops[-1])})"
            L = [f"fcompare(c, ST(0), {s});"]
            if m.endswith("pp"):
                L += ["fpop(c);", "fpop(c);"]
            elif m.endswith("p"):
                L.append("fpop(c);")
            return L
        if m == "ftst":
            return ["fcompare(c, ST(0), 0.0);"]
        if m == "fxch":
            i = stidx(ops[-1]) if ops else 1
            if ops and len(ops) == 2:
                i = stidx(ops[1]) or stidx(ops[0])
            return [f"fxch(c, {i});"]
        if m in ("fcomi", "fcompi", "fucomi", "fucompi"):
            s = f"ST({stidx(ops[-1])})" if ops else "ST(1)"
            L = [f"{{ double a = ST(0), b = {s}; int u = a != a || b != b; zf = u || a == b; pf = u; cf = u || a < b; of = 0; sf = 0; }}"]
            if m.endswith("pi"):
                L.append("fpop(c);")
            return L
        if m == "ffree":
            return []
        if m == "fsincos":
            return ["{ double v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); }"]
        if m == "fnstenv":
            a = self.addr(ins, ops[0])
            return [f"memset(gm + {a}, 0, 28); WR16({a}, c->fcw); WR16({a} + 4, fstsw(c)); WR16({a} + 8, 0xFFFFu);"]
        simple = {
            "fchs": "ST(0) = -ST(0);", "fabs": "ST(0) = fabs(ST(0));", "fsqrt": "ST(0) = sqrt(ST(0));",
            "fsin": "ST(0) = sin(ST(0));", "fcos": "ST(0) = cos(ST(0));",
            "frndint": "ST(0) = fround(c, ST(0));",
            "fld1": "fpush(c, 1.0);", "fldz": "fpush(c, 0.0);", "fldpi": "fpush(c, M_PI);",
            "fldl2e": "fpush(c, M_LOG2E);", "fldln2": "fpush(c, M_LN2);",
            "fptan": "ST(0) = tan(ST(0)); fpush(c, 1.0);",
            "fpatan": "ST(1) = atan2(ST(1), ST(0)); fpop(c);",
            "fprem": "ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x0400;",
            "f2xm1": "ST(0) = exp2(ST(0)) - 1.0;",
            "fscale": "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));",
            "fyl2x": "ST(1) = ST(1) * log2(ST(0)); fpop(c);",
            "fninit": "c->top = 0; c->fsw = 0; c->fcw = 0x37F;",
            "fnclex": "c->fsw &= 0x7F00;",
            "wait": "", "fwait": "",
        }
        if m in simple:
            return [simple[m]]
        if m == "fnstsw":
            o = ops[0]
            if o.type == X.X86_OP_REG:
                return [self.reg_write("ax", "fstsw(c)")]
            return [f"WR16({self.addr(ins, o)}, fstsw(c));"]
        if m == "fldcw":
            return [f"c->fcw = RD16({self.addr(ins, ops[0])});"]
        if m == "fnstcw":
            return [f"WR16({self.addr(ins, ops[0])}, c->fcw);"]
        if m == "fnsave":
            return [f"x86_fnsave(c, {self.addr(ins, ops[0])});"]
        if m == "frstor":
            return [f"x86_frstor(c, {self.addr(ins, ops[0])});"]
        raise Unsupported(m)

    @staticmethod
    def _fset(lines):
        import re
        out = []
        for ln in lines:
            out.append(re.sub(r"ST\((\d)\) = ([^;]+);", r"fset(c, \1, \2);", ln))
        return out

    # ---------- instruction ----------
    def insn(self, f, ins, labels):
        m, ops = ins.mnem, ins.ops
        nxt = ins.addr + ins.size
        o0 = ops[0] if ops else None
        o1 = ops[1] if len(ops) > 1 else None

        def goto(t):
            if t in labels:
                if t <= ins.addr:   # loop back-edge: preemption point for single-core mode (core.c)
                    return f"{{ if (__builtin_expect(g_bgl_contended, 0)) bgl_preempt(); goto L_{t:08X}; }}"
                return f"goto L_{t:08X};"
            if t in self.func_set:
                return f"f_{t:08X}(c); return;"
            return f"x86_trap(c, 0x{t:X}u, \"jump outside function\"); return;"

        if m == "mov" or m == "movsd" and o0 and o0.type == X.X86_OP_REG:
            return [self.wr(ins, o0, self.rd(ins, o1, o0.size))]
        if m == "movzx":
            return [self.wr(ins, o0, f"(uint32_t){self.rd(ins, o1)}")]
        if m == "movsx":
            return [self.wr(ins, o0, f"(uint32_t)(int32_t)({ST_[o1.size]}){self.rd(ins, o1)}")]
        if m == "lea":
            return [self.wr(ins, o0, self.addr(ins, o1).replace(" + c->fs_base", ""))]
        if m in ("add", "adc", "sub", "sbb", "cmp", "and", "or", "xor", "test"):
            return self.arith(ins, m)
        if m in ("inc", "dec"):
            sz = o0.size
            b = BITS[sz]
            op = "+" if m == "inc" else "-"
            ofc = f"r == 0x{1 << (b-1):X}u" if m == "inc" else f"r == 0x{(1 << (b-1)) - 1:X}u"
            return [f"{{ uint32_t r = ({self.rd(ins, o0)} {op} 1u) & {MASK[sz]}; of = {ofc}; {self.szp('r', sz)} {self.wr(ins, o0, 'r')} }}"]
        if m == "neg":
            sz = o0.size
            b = BITS[sz]
            return [f"{{ uint32_t a = {self.rd(ins, o0)}, r = (0u - a) & {MASK[sz]}; cf = a != 0; of = r == 0x{1 << (b-1):X}u; {self.szp('r', sz)} {self.wr(ins, o0, 'r')} }}"]
        if m == "not":
            return [self.wr(ins, o0, f"~{self.rd(ins, o0)}")]
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            return ["{ " + " ".join(self.shift(ins, "shl" if m == "sal" else m)) + " }"]
        if m in ("shld", "shrd"):
            d, s = o0, o1
            cnt = self.rd(ins, ops[2], 1)
            if m == "shld":
                e = "(n ? ((a << n) | (b >> (32 - n))) : a)"
                cf = "(a >> (32 - n)) & 1"
            else:
                e = "(n ? ((a >> n) | (b << (32 - n))) : a)"
                cf = "(a >> (n - 1)) & 1"
            return [f"{{ uint32_t n = ({cnt}) & 31; if (n) {{ uint32_t a = {self.rd(ins, d)}, b = {self.rd(ins, s)}, r = {e}; cf = {cf}; of = 0; {self.szp('r', 4)} {self.wr(ins, d, 'r')} }} }}"]
        if m == "push":
            if o0.type == X.X86_OP_IMM:
                return [f"PUSH32(0x{o0.imm & 0xFFFFFFFF:X}u);"]
            if o0.type == X.X86_OP_REG and self.rn(ins, o0.reg) in SEG:
                return [f"PUSH32((uint32_t){self.rd(ins, o0)});"]
            if o0.size == 2:
                return [f"c->r[ESP] -= 2; WR16(c->r[ESP], {self.rd(ins, o0)});"]
            return [f"PUSH32({self.rd(ins, o0)});"]
        if m == "pop":
            if o0.size == 2 and not (o0.type == X.X86_OP_REG and self.rn(ins, o0.reg) in SEG):
                return [f"{{ uint32_t v = RD16(c->r[ESP]); c->r[ESP] += 2; {self.wr(ins, o0, 'v')} }}"]
            return [f"{{ uint32_t v = POP32(); {self.wr(ins, o0, 'v')} }}"]
        if m == "pushal":
            return ["{ uint32_t sp = c->r[ESP]; PUSH32(c->r[EAX]); PUSH32(c->r[ECX]); PUSH32(c->r[EDX]); PUSH32(c->r[EBX]); PUSH32(sp); PUSH32(c->r[EBP]); PUSH32(c->r[ESI]); PUSH32(c->r[EDI]); }"]
        if m == "popal":
            return ["c->r[EDI] = POP32(); c->r[ESI] = POP32(); c->r[EBP] = POP32(); c->r[ESP] += 4; c->r[EBX] = POP32(); c->r[EDX] = POP32(); c->r[ECX] = POP32(); c->r[EAX] = POP32();"]
        if m == "pushfd":
            return ["PUSH32(cf | (pf << 2) | (zf << 6) | (sf << 7) | (c->df << 10) | (of << 11) | 0x202u | c->eflags_hi);"]
        if m == "popfd":
            return ["{ uint32_t v = POP32(); cf = v & 1; pf = (v >> 2) & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; c->df = (v >> 10) & 1; of = (v >> 11) & 1; c->eflags_hi = v & 0x240000u; }"]
        if m == "lahf":
            return [self.reg_write("ah", "cf | (pf << 2) | (zf << 6) | (sf << 7) | 2u")]
        if m == "sahf":
            return ["{ uint32_t v = c->r[EAX] >> 8; cf = v & 1; pf = (v >> 2) & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; }"]
        if m == "xchg":
            return [f"{{ uint32_t t = {self.rd(ins, o0)}; {self.wr(ins, o0, self.rd(ins, o1))} {self.wr(ins, o1, 't')} }}"]
        if m == "bswap":
            return [self.wr(ins, o0, f"__builtin_bswap32({self.rd(ins, o0)})")]
        if m == "cwde":
            return ["c->r[EAX] = (uint32_t)(int32_t)(int16_t)c->r[EAX];"]
        if m == "cdq":
            return ["c->r[EDX] = ((int32_t)c->r[EAX] < 0) ? 0xFFFFFFFFu : 0u;"]
        if m == "imul":
            if len(ops) == 1:
                sz = o0.size
                if sz == 4:
                    return [f"{{ int64_t p = (int64_t)(int32_t)c->r[EAX] * (int32_t){self.rd(ins, o0)}; c->r[EAX] = (uint32_t)p; c->r[EDX] = (uint32_t)(p >> 32); cf = of = (p != (int32_t)p); }}"]
                if sz == 2:
                    return [f"{{ int32_t p = (int16_t)c->r[EAX] * (int16_t){self.rd(ins, o0)}; {self.reg_write('ax', 'p')} {self.reg_write('dx', 'p >> 16')} cf = of = (p != (int16_t)p); }}"]
                return [f"{{ int32_t p = (int8_t)c->r[EAX] * (int8_t){self.rd(ins, o0)}; {self.reg_write('ax', 'p')} cf = of = (p != (int8_t)p); }}"]
            a = self.rd(ins, o0) if len(ops) == 2 else self.rd(ins, o1)
            b = self.rd(ins, o1) if len(ops) == 2 else self.rd(ins, ops[2], o0.size)
            if len(ops) == 3 and ops[2].type == X.X86_OP_IMM:
                b = f"0x{ops[2].imm & 0xFFFFFFFF:X}u"
            sz = o0.size
            return [f"{{ int64_t p = (int64_t)({ST_[sz]}){a} * ({ST_[sz]}){b}; cf = of = (p != ({ST_[sz]})p); {self.wr(ins, o0, '(uint32_t)p')} }}"]
        if m == "mul":
            sz = o0.size
            if sz == 4:
                return [f"{{ uint64_t p = (uint64_t)c->r[EAX] * {self.rd(ins, o0)}; c->r[EAX] = (uint32_t)p; c->r[EDX] = (uint32_t)(p >> 32); cf = of = c->r[EDX] != 0; }}"]
            if sz == 2:
                return [f"{{ uint32_t p = (uint32_t)(uint16_t)c->r[EAX] * {self.rd(ins, o0)}; {self.reg_write('ax', 'p')} {self.reg_write('dx', 'p >> 16')} cf = of = (p >> 16) != 0; }}"]
            return [f"{{ uint32_t p = (uint32_t)(uint8_t)c->r[EAX] * {self.rd(ins, o0)}; {self.reg_write('ax', 'p')} cf = of = (p >> 8) != 0; }}"]
        if m in ("div", "idiv"):
            sz = o0.size
            s = m == "idiv"
            if sz == 4:
                if s:
                    return [f"{{ int64_t n = (int64_t)(((uint64_t)c->r[EDX] << 32) | c->r[EAX]); int32_t d = (int32_t){self.rd(ins, o0)}; if (!d) {{ x86_trap(c, 0x{ins.addr:X}u, \"#DE\"); return; }} c->r[EAX] = (uint32_t)(int32_t)(n / d); c->r[EDX] = (uint32_t)(int32_t)(n % d); }}"]
                return [f"{{ uint64_t n = ((uint64_t)c->r[EDX] << 32) | c->r[EAX]; uint32_t d = {self.rd(ins, o0)}; if (!d) {{ x86_trap(c, 0x{ins.addr:X}u, \"#DE\"); return; }} c->r[EAX] = (uint32_t)(n / d); c->r[EDX] = (uint32_t)(n % d); }}"]
            if sz == 2:
                if s:
                    return [f"{{ int32_t n = (int32_t)(((c->r[EDX] & 0xFFFF) << 16) | (c->r[EAX] & 0xFFFF)); int16_t d = (int16_t){self.rd(ins, o0)}; if (!d) {{ x86_trap(c, 0x{ins.addr:X}u, \"#DE\"); return; }} {self.reg_write('ax', 'n / d')} {self.reg_write('dx', 'n % d')} }}"]
                return [f"{{ uint32_t n = ((c->r[EDX] & 0xFFFF) << 16) | (c->r[EAX] & 0xFFFF); uint16_t d = {self.rd(ins, o0)}; if (!d) {{ x86_trap(c, 0x{ins.addr:X}u, \"#DE\"); return; }} {self.reg_write('ax', 'n / d')} {self.reg_write('dx', 'n % d')} }}"]
            if s:
                return [f"{{ int16_t n = (int16_t)c->r[EAX]; int8_t d = (int8_t){self.rd(ins, o0)}; if (!d) {{ x86_trap(c, 0x{ins.addr:X}u, \"#DE\"); return; }} {self.reg_write('al', 'n / d')} {self.reg_write('ah', 'n % d')} }}"]
            return [f"{{ uint16_t n = (uint16_t)c->r[EAX]; uint8_t d = {self.rd(ins, o0)}; if (!d) {{ x86_trap(c, 0x{ins.addr:X}u, \"#DE\"); return; }} {self.reg_write('al', 'n / d')} {self.reg_write('ah', 'n % d')} }}"]
        if m.startswith("set"):
            cond = COND["j" + m[3:]]
            return [self.wr(ins, o0, f"{cond} ? 1u : 0u")]
        if m in ("bt", "bts", "btr", "btc"):
            if o0.type == X.X86_OP_MEM and o1.type == X.X86_OP_REG:
                ba = f"({self.addr(ins, o0)} + (uint32_t)(((int32_t){self.rd(ins, o1)} >> 5) * 4))"
                L = [f"{{ uint32_t a = {ba}, v = RD32(a), k = {self.rd(ins, o1)} & 31; cf = (v >> k) & 1;"]
                op = {"bts": "v | (1u << k)", "btr": "v & ~(1u << k)", "btc": "v ^ (1u << k)"}.get(m)
                if op:
                    L.append(f"WR32(a, {op});")
                return [" ".join(L) + " }"]
            bit = f"(({self.rd(ins, o1, 1 if o1.type == X.X86_OP_IMM else o1.size)}) & 31)"
            L = [f"{{ uint32_t v = {self.rd(ins, o0)}, k = {bit}; cf = (v >> k) & 1;"]
            if m == "bts":
                L.append(self.wr(ins, o0, "v | (1u << k)"))
            elif m == "btr":
                L.append(self.wr(ins, o0, "v & ~(1u << k)"))
            elif m == "btc":
                L.append(self.wr(ins, o0, "v ^ (1u << k)"))
            L.append("}")
            return [" ".join(L)]
        if m in ("bsf", "bsr"):
            fn = "__builtin_ctz(v)" if m == "bsf" else "31 - __builtin_clz(v)"
            return [f"{{ uint32_t v = {self.rd(ins, o1)}; zf = v == 0; if (v) {{ {self.wr(ins, o0, fn)} }} }}"]
        if m == "les":
            a = self.addr(ins, o1)
            return [self.wr(ins, o0, f"RD32({a})")]  # es selector ignored (flat model)
        if m == "leave":
            return ["c->r[ESP] = c->r[EBP]; c->r[EBP] = POP32();"]
        if m == "clc":
            return ["cf = 0;"]
        if m == "stc":
            return ["cf = 1;"]
        if m == "cmc":
            return ["cf ^= 1;"]
        if m == "cld":
            return ["c->df = 0;"]
        if m == "std":
            return ["c->df = 1;"]
        if m in ("nop", "wait", "fwait"):
            return []
        if m == "cpuid":
            return ["x86_cpuid(c);"]
        if m == "rdtsc":
            return ["x86_rdtsc(c);"]
        if m == "emms":
            return []
        if m.startswith("cmov") and ("j" + m[4:]) in COND:
            return [f"if {COND['j' + m[4:]]} {{ {self.wr(ins, o0, self.rd(ins, o1, o0.size))} }}"]
        if m == "enter":
            n, lvl = o0.imm & 0xFFFF, (o1.imm & 0x1F) if o1 else 0
            if lvl:
                raise Unsupported("enter with nesting level")
            return [f"PUSH32(c->r[EBP]); c->r[EBP] = c->r[ESP]; c->r[ESP] -= {n}u;"]
        if m == "xlatb":
            return [self.reg_write("al", "RD8(c->r[EBX] + (c->r[EAX] & 0xFFu))")]
        if m == "movd":
            def mmr(op):
                n = self.rn(ins, op.reg)
                return int(n[2]) if n.startswith("mm") else None
            if o0.type == X.X86_OP_REG and mmr(o0) is not None:
                return [f"c->mm[{mmr(o0)}] = (uint64_t)(uint32_t){self.rd(ins, o1, 4)};"]
            return [self.wr(ins, o0, f"(uint32_t)c->mm[{mmr(o1)}]", 4)]
        if m in MMX_OPS:
            d = int(self.rn(ins, o0.reg)[2])
            if o1.type == X.X86_OP_REG:
                src = f"c->mm[{int(self.rn(ins, o1.reg)[2])}]"
            elif o1.type == X.X86_OP_IMM:
                src = f"(uint64_t)0x{o1.imm & 0xFF:X}u"
            else:
                src = f"RD64({self.addr(ins, o1)})"
            return [f"c->mm[{d}] = mmx_{m}(c->mm[{d}], {src});"]
        if m == "movq":
            def mm(op):
                return int(self.rn(ins, op.reg)[2]) if op.type == X.X86_OP_REG else None
            if o0.type == X.X86_OP_REG:
                src = f"c->mm[{mm(o1)}]" if o1.type == X.X86_OP_REG else f"RD64({self.addr(ins, o1)})"
                return [f"c->mm[{mm(o0)}] = {src};"]
            return [f"WR64({self.addr(ins, o0)}, c->mm[{mm(o1)}]);"]
        if m == "int":
            return [f"x86_int(c, 0x{ins.addr:X}u, {o0.imm & 0xFF}u);"]
        if m == "int3":
            return [f"x86_trap(c, 0x{ins.addr:X}u, \"int3\"); return;"]
        if m.split(" ")[-1][:4] in ("movs", "stos", "lods", "scas", "cmps"):
            return self.strop(ins)
        if m.startswith("f"):
            return self._fset(self.fpu(ins))

        # ---- control flow ----
        if m == "call":
            L = [f"PUSH32(0x{nxt:X}u);"]
            if o0.type == X.X86_OP_IMM:
                t = o0.imm & 0xFFFFFFFF
                if t in self.func_set:
                    L.append(f"f_{t:08X}(c);")
                else:
                    L.append(f"call_indirect(c, 0x{t:X}u);")
            else:
                # evaluate the target before pushing: `call [esp]` reads the pre-push slot
                return [f"{{ uint32_t t = {self.rd(ins, o0, 4)}; PUSH32(0x{nxt:X}u); call_indirect(c, t); }}"]
            return L
        if m == "ret":
            n = 4 + (o0.imm if o0 else 0)
            return [f"c->r[ESP] += {n}; TRACE_LEAVE(0x{f.start:X}u); return;"]
        if m == "jmp":
            if o0.type == X.X86_OP_IMM:
                return [goto(o0.imm & 0xFFFFFFFF)]
            if ins.addr in f.jump_tables:
                tgt = self.rd(ins, o0, 4)
                cases = " ".join(f"case 0x{t:X}u: goto L_{t:08X};" for t in sorted(set(f.jump_tables[ins.addr])))
                return [f"switch ({tgt}) {{ {cases} default: call_indirect(c, {tgt}); return; }}"]
            return [f"call_indirect(c, {self.rd(ins, o0, 4)}); return;"]  # tail call
        if m in COND:
            return [f"if {COND[m]} {goto(o0.imm & 0xFFFFFFFF)}"]
        if m in ("jecxz", "jcxz"):
            return [f"if (c->r[ECX] == 0) {goto(o0.imm & 0xFFFFFFFF)}"]
        if m in ("loop", "loope", "loopne"):
            extra = {"loop": "", "loope": " && zf", "loopne": " && !zf"}[m]
            return [f"if (--c->r[ECX] != 0{extra}) {goto(o0.imm & 0xFFFFFFFF)}"]
        raise Unsupported(m)

    def function(self, f):
        return promote_registers(self.function_raw(f))

    def function_raw(self, f):
        labels = set(f.blocks)
        out = [f"void f_{f.start:08X}(CPU *c) {{",
               "    uint32_t cf = c->cf, zf = c->zf, sf = c->sf, of = c->of, pf = c->pf;",
               "    (void)cf; (void)zf; (void)sf; (void)of; (void)pf;",
               f"    TRACE_ENTER(0x{f.start:X}u);",
               "    LOAD_REGS();",
               f"    goto L_{f.start:08X};"]
        for b in sorted(f.blocks):
            ins_list = f.blocks[b]
            out.append(f"L_{b:08X}:;")
            for ins in ins_list:
                try:
                    code = self.insn(f, ins, labels)
                except Unsupported as e:
                    self.unsupported[ins.mnem] += 1
                    code = [f"x86_trap(c, 0x{ins.addr:X}u, \"unsupported: {ins.text}\"); return; /* {e} */"]
                out.append(f"    /* {ins.addr:08X} {ins.text} */")
                out.extend("    " + line for line in code)
            last = ins_list[-1] if ins_list else None
            if last is None:
                out.append("    return;")
            elif last.mnem not in ("ret", "jmp", "int3") or last.mnem == "jmp" and False:
                nxt = last.addr + last.size
                out.append("    " + (f"goto L_{nxt:08X};" if nxt in labels
                                     else f"x86_trap(c, 0x{nxt:X}u, \"fell off block\"); return;"))
        out.append("}")
        return "\n".join(out)

# ---------- register promotion ----------
# Guest registers live in C locals r0..r7 inside each lifted function, so the
# compiler can keep them in host registers (guest memory stores through gm can
# no longer alias them). They are written back to c->r[] (SPILL) before anything
# that reads or writes the CPU struct's registers: calls to other lifted
# functions, call_indirect (thunks, host code), traps/cpuid/rdtsc/int, and
# returns; and reloaded (RELOAD) after calls that come back.
_REGIDX = {"EAX": 0, "ECX": 1, "EDX": 2, "EBX": 3, "ESP": 4, "EBP": 5, "ESI": 6, "EDI": 7}
_REG_RE = re.compile(r"c->r\[(\d|EAX|ECX|EDX|EBX|ESP|EBP|ESI|EDI)\]")
# helpers that take the CPU but never touch integer registers
_PURE = {"fpop", "fpush", "fset", "fxch", "fstsw", "fcompare", "fround", "fpush_st", "fcopy", "fild64", "fist64",
         "x86_fnsave", "x86_frstor"}
_CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\(c(\)|,[^;]*\));( return;)?")

def promote_registers(text):
    def reg(m):
        g = m.group(1)
        return f"r{_REGIDX[g] if g in _REGIDX else int(g)}"
    text = _REG_RE.sub(reg, text)
    def call(m):
        name = m.group(1)
        if name in _PURE:
            return m.group(0)
        callexpr = f"{name}(c{m.group(2)};"
        if m.group(3):            # tail position: nothing to reload
            return f"{{ SPILL(); {callexpr} @RET@ }}"
        return f"{{ SPILL(); {callexpr} RELOAD(); }}"
    text = _CALL_RE.sub(call, text)
    # remaining returns (ret instructions etc.): write registers back first
    text = re.sub(r"TRACE_LEAVE\((0x[0-9A-F]+u)\); return;", r"{ SPILL(); TRACE_LEAVE(\1); @RET@ }", text)
    text = re.sub(r"\breturn;", "{ SPILL(); return; }", text).replace("@RET@", "return;")
    # sanity: no stray CPU register access, and every return spills
    assert "c->r[" not in text
    return text


def main(exe, outdir, seeds=(), modname="nfs4", base=None):
    img = Image(exe, base)
    prog = Program(img)
    prog.run(seeds)
    em = Emitter(prog)
    os.makedirs(outdir, exist_ok=True)
    funcs = sorted(prog.funcs.values(), key=lambda f: f.start)
    hdr = ["#pragma once", '#include "cpu.h"', '#include "lifted.h"'] + [f"void f_{f.start:08X}(CPU *c);" for f in funcs]
    with open(os.path.join(outdir, "funcs.h"), "w") as fp:
        fp.write("\n".join(hdr) + "\n")
    CHUNK = 150
    nfiles = 0
    for i in range(0, len(funcs), CHUNK):
        with open(os.path.join(outdir, f"code_{i // CHUNK:03d}.c"), "w") as fp:
            fp.write('#include "funcs.h"\n\n')
            for f in funcs[i : i + CHUNK]:
                fp.write(em.function(f) + "\n\n")
        nfiles += 1
    with open(os.path.join(outdir, "dispatch.c"), "w") as fp:
        fp.write('#include "funcs.h"\n#include <stddef.h>\n')
        fp.write(f"const struct {{ uint32_t addr; gfn fn; }} g_functab_{modname}[] = {{\n")
        fp.write("".join(f"  {{0x{f.start:X}u, f_{f.start:08X}}},\n" for f in funcs))
        fp.write("};\n")
        fp.write(f"const size_t g_functab_{modname}_len = {len(funcs)};\n")
    n = sum(len(b) for f in funcs for b in f.blocks.values())
    bad = sum(em.unsupported.values())
    print(f"lifted {len(funcs)} functions, {n} instructions into {nfiles} files")
    print(f"unsupported: {bad} ({100 * bad / max(n, 1):.3f}%)  {dict(em.unsupported.most_common(20))}")


if __name__ == "__main__":
    seeds = []
    if "--seeds" in sys.argv:
        img_lo = Image(sys.argv[1], int(sys.argv[sys.argv.index("--base") + 1], 16) if "--base" in sys.argv else None)
        with open(sys.argv[sys.argv.index("--seeds") + 1]) as fp:
            seeds = [a for a in (int(l, 16) for l in fp if l.strip()) if img_lo.is_code(a)]
    name = sys.argv[sys.argv.index("--name") + 1] if "--name" in sys.argv else "nfs4"
    base = int(sys.argv[sys.argv.index("--base") + 1], 16) if "--base" in sys.argv else None
    main(sys.argv[1], sys.argv[2], seeds, name, base)
