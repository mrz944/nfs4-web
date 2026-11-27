/* Hooks: lifted functions replaced by runtime C (runtime/hooks.txt, tools/lift.py --hooks).
 * f_<addr> calls hook_<name>(c) at the function's entry: the guest return address is at
 * [esp], stack arguments from [esp + 4] up, register arguments in c->r (Watcom: eax, edx, ebx,
 * ecx, then the stack; the callee pops with `ret N`). A hook either calls f_<addr>_orig(c),
 * which runs the original from that same state (and returns, popping what it pops), or does the
 * work itself and returns like the original would: same registers kept, the return address and
 * N bytes of arguments popped, a float result pushed on the x87 stack. */
#pragma once
#include "rt.h"

#define HOOK_ARG32(c, i)  RD32((c)->r[ESP] + 4 + 4 * (i))          /* i-th stack argument */
#define HOOK_ARGF(c, i)   RDF32((c)->r[ESP] + 4 + 4 * (i))

void f_00453F10_orig(CPU *c);
