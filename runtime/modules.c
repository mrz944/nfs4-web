/* Dynamically loaded modules (LoadLibrary/GetProcAddress) and WSOCK32.
 *
 * The game loads its renderer ("thrash" driver) and EA's sound library at
 * run time. Each is a host module here: GetProcAddress returns a thunk; the
 * stdcall arg count comes from the decorated name (_name@N). Functions with
 * a real implementation are listed in the per-module tables; anything else
 * is traced and returns 0 so the call protocol can be observed.
 */
#define _GNU_SOURCE
#include "rt.h"
#include <ctype.h>

#define HMOD_THRASH 0x70000000u
#define HMOD_EACSND 0x71000000u
#define HMOD_GLIDE  0x72000000u
#define HMOD_SYS    0x73000000u   /* + index * 0x10000: runtime-implemented system DLLs */
static const char *const sys_dlls[] = { "DDRAW.DLL", "DSOUND.DLL", "DINPUT.DLL", "WINMM.DLL", "USER32.DLL",
    "GDI32.DLL", "ADVAPI32.DLL", "VERSION.DLL", "WSOCK32.DLL", "SHELL32.DLL", "OLE32.DLL", NULL };

typedef struct { const char *name; hostfn fn; } ModFn;
extern const ModFn thrash_fns[];   /* thrash.c */
extern const ModFn eacsnd_fns[];   /* eacsnd.c */
extern const ModFn glide_fns[];    /* glide.c: Glide 3 for the lifted voodoo2a.dll thrash driver */

static const char *base_name(const char *p) {
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') s = q + 1;
    return s;
}

#define MAX_IMAGES 8
static struct { char name[64]; uint32_t base; } images[MAX_IMAGES];
static int nimages;

static uint32_t image_load(const char *path, const char *b) {
    for (int i = 0; i < nimages; i++) if (!strcasecmp(images[i].name, b)) return images[i].base;
    char hp[1024]; fs_map(path, hp, sizeof hp);
    uint32_t entry, base = load_pe_at(hp, &entry, lifted_base(b));
    snprintf(images[nimages].name, sizeof images[0].name, "%s", b);
    images[nimages++].base = base;
    if (entry) {
        CPU *c = tls_cpu;
        uint32_t ok = guest_call(c, entry, 3, base, 1u /* DLL_PROCESS_ATTACH */, 0u);
        rt_log("%s DllMain -> %u", b, ok);
    }
    return base;
}
static int is_image(uint32_t h) {
    for (int i = 0; i < nimages; i++) if (images[i].base == h) return 1;
    return 0;
}

uint32_t module_load(const char *path) {
    const char *b = base_name(path);
    /* real lifted DLL available (e.g. the software thrash driver)? */
    if (lifted_base(b)) return image_load(path, b);
    /* every thrash driver DLL (dx5a/dx6a/dx7a/dx8a/voodoo2a/softtria) maps to our renderer */
    size_t n = strlen(b);
    if (n > 5 && !strcasecmp(b + n - 5, "a.dll") &&
        (!strncasecmp(b, "dx", 2) || !strncasecmp(b, "voodoo", 6) || !strncasecmp(b, "softtri", 7)))
        return HMOD_THRASH;
    if (!strcasecmp(b, "eacsnd.dll")) return HMOD_EACSND;
    if (!strcasecmp(b, "glide3x.dll")) return HMOD_GLIDE;
    /* system DLLs the runtime implements (NFS4 probes DDRAW.DLL for DirectX 6) */
    for (int i = 0; sys_dlls[i]; i++)
        if (!strcasecmp(b, sys_dlls[i]) || (!strncasecmp(b, sys_dlls[i], strlen(b)) && !strcasecmp(sys_dlls[i] + strlen(b), ".dll")))
            return HMOD_SYS + (uint32_t)i * 0x10000u;
    return 0;
}

static __thread const char *trace_name;
typedef struct { char name[64]; int nargs; } TraceInfo;
static TraceInfo traces[256]; static int ntraces;

static uint32_t trace_stub(CPU *c, const uint32_t *a) {
    /* recover which function this is from the thunk return path is not
     * possible, so each traced function gets its own small wrapper below */
    (void)c; (void)a; return 0;
}

#define TRACE_SLOTS 64
static uint32_t trace_call(int slot, CPU *c, const uint32_t *a) {
    TraceInfo *t = &traces[slot];
    char buf[256]; int k = snprintf(buf, sizeof buf, "%s(", t->name);
    for (int i = 0; i < t->nargs && i < 8; i++) k += snprintf(buf + k, sizeof buf - k, "%s%#x", i ? ", " : "", a[i]);
    snprintf(buf + k, sizeof buf - k, ") [unimplemented] -> 0");
    rt_log("%s", buf);
    (void)c;
    return 0;
}
#define TS(x, n) static uint32_t trace_##x##_##n(CPU *c, const uint32_t *a) { return trace_call(x * 8 + n, c, a); }
#define R8(x) TS(x,0) TS(x,1) TS(x,2) TS(x,3) TS(x,4) TS(x,5) TS(x,6) TS(x,7)
R8(0) R8(1) R8(2) R8(3) R8(4) R8(5) R8(6) R8(7)
#undef R8
#define R8(x) trace_##x##_0, trace_##x##_1, trace_##x##_2, trace_##x##_3, trace_##x##_4, trace_##x##_5, trace_##x##_6, trace_##x##_7
static const hostfn trace_fns[TRACE_SLOTS] = { R8(0), R8(1), R8(2), R8(3), R8(4), R8(5), R8(6), R8(7) };

/* RT_THRASH_SPY=N: wrap the lifted softtria exports; log the first N calls of each
 * function with its stack args and the 64 bytes behind pointer args, then pass the
 * call through. Every 600 pageflips prints per-function call counts. (Renderer RE.) */
#define SPY_SLOTS 64
static struct { char name[64]; uint32_t real; int nargs; uint64_t calls; } spy[SPY_SLOTS];
static int nspy, spy_limit = -1;
static void spy_dump_ptr(const char *what, uint32_t p) {
    if (p < 0x10000 || p >= GUEST_SIZE - 64) return;
    char h[400]; int k = 0;
    for (int i = 0; i < 64; i += 4) {
        uint32_t v; memcpy(&v, G + p + i, 4); float f; memcpy(&f, &v, 4);
        uint32_t e = v & 0x7f800000;
        if (e && e != 0x7f800000 && f > -1e6f && f < 1e6f && (f > 1e-4f || f < -1e-4f)) k += snprintf(h + k, sizeof h - k, "%g ", (double)f);
        else k += snprintf(h + k, sizeof h - k, "%#x ", v);
    }
    rt_log("      %s @%#x: %s", what, p, h);
}
static uint32_t spy_call(int slot, CPU *c, const uint32_t *a) {
    if (spy_limit < 0) spy_limit = atoi(getenv("RT_THRASH_SPY"));
    uint64_t n = ++spy[slot].calls;
    if (n <= (uint64_t)spy_limit) {
        char buf[256]; int k = snprintf(buf, sizeof buf, "THRASH %s(", spy[slot].name);
        for (int i = 0; i < spy[slot].nargs; i++) k += snprintf(buf + k, sizeof buf - k, "%s%#x", i ? ", " : "", a[i]);
        rt_log("%s)", buf);
        for (int i = 0; i < spy[slot].nargs; i++) { char w[8]; snprintf(w, sizeof w, "arg%d", i); spy_dump_ptr(w, a[i]); }
    }
    uint32_t r;
    switch (spy[slot].nargs) {
    case 0: r = guest_call(c, spy[slot].real, 0); break;
    case 1: r = guest_call(c, spy[slot].real, 1, a[0]); break;
    case 2: r = guest_call(c, spy[slot].real, 2, a[0], a[1]); break;
    case 3: r = guest_call(c, spy[slot].real, 3, a[0], a[1], a[2]); break;
    case 4: r = guest_call(c, spy[slot].real, 4, a[0], a[1], a[2], a[3]); break;
    default: r = guest_call(c, spy[slot].real, 5, a[0], a[1], a[2], a[3], a[4]); break;
    }
    if (n <= (uint64_t)spy_limit) { rt_log("       = %#x", r); if (!strncmp(spy[slot].name, "_THRASH_about", 13) || !strncmp(spy[slot].name, "_THRASH_lockwindow", 18)) spy_dump_ptr("ret", r); }
    if (!strncmp(spy[slot].name, "_THRASH_pageflip", 16) || !strncmp(spy[slot].name, "_THRASH_sync", 12)) {
        static struct { uint32_t ra; uint64_t n; } callers[32]; static int ncallers;
        uint32_t ra = RD32(c->r[ESP]); if (ra == 0x434b51 || ra == 0x434b2d) ra = RD32(c->r[ESP] + 16);
        int i; for (i = 0; i < ncallers && callers[i].ra != ra; i++) {}
        if (i == ncallers && ncallers < 32) callers[ncallers++].ra = ra;
        if (i < 32) callers[i].n++;
        if (n % 50000 == 0) { char b[512]; int k = snprintf(b, sizeof b, "%s callers:", spy[slot].name);
            for (int j = 0; j < ncallers; j++) k += snprintf(b + k, sizeof b - k, " %#x=%llu", callers[j].ra, (unsigned long long)callers[j].n);
            rt_log("%s", b); }
    }
    if (!strncmp(spy[slot].name, "_THRASH_pageflip", 16) && n % 600 == 0) {
        char buf[2048]; int k = snprintf(buf, sizeof buf, "THRASH calls after %llu flips:", (unsigned long long)n);
        for (int i = 0; i < nspy; i++) if (spy[i].calls) k += snprintf(buf + k, sizeof buf - k, " %s=%llu", spy[i].name + 8, (unsigned long long)spy[i].calls);
        rt_log("%s", buf);
    }
    return r;
}
#define SS(x, n) static uint32_t spy_##x##_##n(CPU *c, const uint32_t *a) { return spy_call(x * 8 + n, c, a); }
#define R8(x) SS(x,0) SS(x,1) SS(x,2) SS(x,3) SS(x,4) SS(x,5) SS(x,6) SS(x,7)
R8(0) R8(1) R8(2) R8(3) R8(4) R8(5) R8(6) R8(7)
#undef R8
#define R8(x) spy_##x##_0, spy_##x##_1, spy_##x##_2, spy_##x##_3, spy_##x##_4, spy_##x##_5, spy_##x##_6, spy_##x##_7
static const hostfn spy_fns[SPY_SLOTS] = { R8(0), R8(1), R8(2), R8(3), R8(4), R8(5), R8(6), R8(7) };
#undef R8

uint32_t module_proc(uint32_t hmod, const char *name) {
    if (is_image(hmod)) {
        uint32_t p = pe_export(hmod, name);
        const char *at = strrchr(name, '@');
        if (p && getenv("RT_THRASH_SPY") && !strncmp(name, "_THRASH_", 8) && at && nspy < SPY_SLOTS) {
            int s = nspy++;
            snprintf(spy[s].name, sizeof spy[s].name, "%s", name);
            spy[s].real = p; spy[s].nargs = atoi(at + 1) / 4;
            return thunk_new(name, spy_fns[s], spy[s].nargs);
        }
        return p;
    }
    if (hmod >= HMOD_SYS && hmod < HMOD_SYS + 0x10000u * (sizeof sys_dlls / sizeof *sys_dlls - 1) && !(hmod & 0xFFFF)) {
        const char *dll = sys_dlls[(hmod - HMOD_SYS) >> 16];
        return import_find(dll, name) ? import_thunk(dll, name) : 0;
    }
    const ModFn *tab = hmod == HMOD_THRASH ? thrash_fns : hmod == HMOD_EACSND ? eacsnd_fns : hmod == HMOD_GLIDE ? glide_fns : NULL;
    if (!tab) return 0;
    int nargs = 0;
    const char *at = strrchr(name, '@');
    if (at) nargs = atoi(at + 1) / 4;
    const char *mod = hmod == HMOD_THRASH ? "THRASH" : hmod == HMOD_GLIDE ? "GLIDE" : "EACSND";
    char full[128]; snprintf(full, sizeof full, "%s!%s", mod, name);
    for (const ModFn *f = tab; f->name; f++)
        if (!strcmp(f->name, name)) return thunk_new(full, f->fn, nargs);
    if (ntraces >= TRACE_SLOTS) rt_fatal("too many traced module functions");
    int s = ntraces++;
    snprintf(traces[s].name, sizeof traces[s].name, "%s", full);
    traces[s].nargs = nargs;
    (void)trace_stub; (void)trace_name;
    return thunk_new(full, trace_fns[s], nargs);
}

/* WSOCK32 lives in net.c */

void kernel32_init(void);
void win32_init(void) { kernel32_init(); }
