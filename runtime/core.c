/* Core runtime: guest memory, allocator, thunks, dispatch, PE loader, entry. */
#define _GNU_SOURCE
#include "rt.h"
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <errno.h>
#include <math.h>

uint8_t *G;
double rt_tick_ms;
void rt_tick_round(struct timespec *dl) {
    if (rt_tick_ms <= 0) return;
    double t = (double)dl->tv_sec * 1e3 + dl->tv_nsec / 1e6, tick = rt_tick_ms;
    t = ceil(t / tick) * tick;
    dl->tv_sec = (time_t)(t / 1e3); dl->tv_nsec = (long)((t - (double)dl->tv_sec * 1e3) * 1e6);
    if (dl->tv_nsec >= 1000000000) { dl->tv_sec++; dl->tv_nsec -= 1000000000; }
}
void rt_usleep(unsigned us) {
    if (rt_tick_ms <= 0) { usleep(us); return; }
    struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += us / 1000000; dl.tv_nsec += (long)(us % 1000000) * 1000;
    if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    rt_tick_round(&dl);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL) == EINTR) {}
}
int rt_verbose = 1;
__thread CPU *tls_cpu;
__thread const char *tls_thunk_name;
/* watchdog (rt_watch_*): what every guest thread is doing, readable from another thread */
static __thread volatile uint32_t tls_calls, tls_ret;
static __thread const char *volatile tls_last;
typedef struct { uint32_t tid; CPU *const volatile *cpu; const char *const volatile *in; const char *const volatile *last; volatile uint32_t *calls, *ret; uint32_t seen; } Watch;
static Watch watch[64]; static int nwatch;
uint32_t rt_caller(void) { return tls_ret; }   /* guest address of the current API call */
void guest_backtrace(CPU *c);
static __thread uint32_t tls_tid;
void rt_watch_thread(uint32_t tid) {
    tls_tid = tid;
    int i = __atomic_fetch_add(&nwatch, 1, __ATOMIC_SEQ_CST);
    if (i < 64) watch[i] = (Watch){ tid, (CPU *const volatile *)&tls_cpu, (const char *const volatile *)&tls_thunk_name, &tls_last, &tls_calls, &tls_ret, 0 };
}
void bgl_dump(void);
void rt_watch_dump(const char *why) {
    bgl_dump();
    extern void ev_dump(void); ev_dump();
    for (const char *pk = getenv("RT_WATCH_PEEK"); pk && *pk; ) {   /* hexaddr:len,... */
        unsigned pa = 0, pl = 16; int used = 0;
        if (sscanf(pk, "%x:%u%n", &pa, &pl, &used) < 2 || !used) break;
        if (pl > 64) pl = 64;
        char h[256]; int k = 0;
        for (unsigned i = 0; i < pl; i++) k += snprintf(h + k, sizeof h - k, "%02x%s", G[pa + i], (i & 3) == 3 ? " " : "");
        rt_log("  [%#x] %s", pa, h);
        pk += used; if (*pk == ',') pk++;
    }
    rt_log("threads (%s):", why);
    int n = nwatch < 64 ? nwatch : 64;
    for (int i = 0; i < n; i++) {
        Watch *w = &watch[i];
        uint32_t calls = *w->calls, d = calls - w->seen; w->seen = calls;
        const char *in = *w->in, *last = *w->last;
        rt_log("  thread %#x: %s %s, from %#x, %u calls since last look", w->tid, in ? "in" : "running guest code, last call", in ? in : last ? last : "-", *w->ret, d);
        if (*w->cpu) guest_backtrace(*w->cpu);
    }
}
__thread uint32_t tls_lasterr;
char g_game_dir[1024];

static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
void rt_log(const char *fmt, ...) {
    if (!rt_verbose) return;
    va_list ap; va_start(ap, fmt);
    static struct timespec t0; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    pthread_mutex_lock(&log_mu);
    if (!t0.tv_sec) t0 = t;
    fprintf(stderr, "[rt %7.3f] ", (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    pthread_mutex_unlock(&log_mu);
    va_end(ap);
}
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[rt] FATAL: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

/* ------------------------------------------------------------------ heap */
/* Size-class free lists + bump pointer, header holds the block size.     */
static pthread_mutex_t heap_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t heap_top = HEAP_LO;
#define NCLASS 40
static uint32_t freelist[NCLASS];

static int size_class(uint32_t n) { /* power-of-two classes from 16 bytes */
    int k = 4; while ((1u << k) < n) k++; return k;
}
uint32_t galloc(uint32_t size) {
    if (size == 0) size = 1;
    uint32_t need = size + 16;
    int k = size_class(need);
    uint32_t bsz = 1u << k, p;
    pthread_mutex_lock(&heap_mu);
    if (freelist[k]) {
        p = freelist[k];
        freelist[k] = RD32(p);
    } else {
        if ((uint64_t)heap_top + bsz > HEAP_HI) { pthread_mutex_unlock(&heap_mu); return 0; }
        p = heap_top; heap_top += bsz;
    }
    pthread_mutex_unlock(&heap_mu);
    memset(G + p, 0, bsz);
    WR32(p, (uint32_t)k); WR32(p + 4, size);
    return p + 16;
}
void gfree(uint32_t p) {
    if (!p) return;
    uint32_t h = p - 16; int k = (int)RD32(h);
    if (k < 4 || k >= NCLASS) { rt_log("gfree: bad block %#x", p); return; }
    pthread_mutex_lock(&heap_mu);
    WR32(h, freelist[k]); freelist[k] = h;
    pthread_mutex_unlock(&heap_mu);
}
uint32_t heap_used(void) { return heap_top - HEAP_LO; }
uint32_t gsize(uint32_t p) { return p ? RD32(p - 12) : 0; }
uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return galloc(size);
    uint32_t old = gsize(p);
    uint32_t cap = (1u << RD32(p - 16)) - 16;
    if (size <= cap) { WR32(p - 12, size); return p; }
    uint32_t n = galloc(size);
    if (n) { memcpy(G + n, G + p, old < size ? old : size); gfree(p); }
    return n;
}
uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = galloc(n);
    memcpy(G + p, s, n); return p;
}

/* ---------------------------------------------------------------- thunks */
typedef struct { const char *name; hostfn fn; int nargs; int blocks; } Thunk;
#define MAX_THUNKS 4096
static Thunk thunks[MAX_THUNKS];
static int nthunks;
static pthread_mutex_t thunk_mu = PTHREAD_MUTEX_INITIALIZER;


/* ---------------------------------------------------- single-core mode
 * NFS4 is not safe on a multi-core CPU: e.g. its file streamer tests a chunk's
 * state before taking the lock (STREAM_release "chunk has already been released").
 * On Windows the modern patch pins it to one core (SingleProcAffinity=1). Here
 * the same: one guest thread runs at a time. A thread hands over at API calls
 * that can block, and at any API call once it has run for a time slice while
 * others are waiting. Like Windows' scheduler, the lock goes to the waiting thread
 * with the highest priority (SetThreadPriority; FIFO among equals), and a thread
 * hands over at once when a higher-priority one is waiting. A thread that has
 * waited 50 ms goes first regardless (Windows boosts starved threads too).
 * RT_MULTICORE=1: off. */
static pthread_mutex_t bgl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bgl_cv = PTHREAD_COND_INITIALIZER;
#define BGL_MAXW 64
static struct { int used, prio; uint64_t seq, since; } bgl_w[BGL_MAXW];
static int bgl_nw, bgl_busy; static uint64_t bgl_seq, bgl_grants;
static int bgl_on = -1;
volatile int g_bgl_contended;   /* someone is waiting for the lock */
static volatile int bgl_top_wait = -1000;   /* highest priority among the waiters */
static volatile int bgl_owner_prio;
static __thread int bgl_held;
static __thread uint64_t bgl_since;
static int bgl_prio_default;
static __thread volatile int *bgl_prio_ptr;  /* this thread's priority (Thread.prio) */
void bgl_set_prio_ptr(volatile int *p) { bgl_prio_ptr = p; }
static int bgl_my_prio(void) { return bgl_prio_ptr ? *bgl_prio_ptr : bgl_prio_default; }
static volatile long bgl_owner; static const char *volatile bgl_owner_call;
#include <unistd.h>
static uint64_t bgl_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
static int bgl_enabled(void) { if (bgl_on < 0) bgl_on = !(getenv("RT_MULTICORE") && atoi(getenv("RT_MULTICORE"))); return bgl_on; }
static void stall_add(const char *name, uint64_t us);
static void bgl_recount(void) {     /* under bgl_mu */
    int top = -1000;
    for (int i = 0; i < BGL_MAXW; i++) if (bgl_w[i].used && bgl_w[i].prio > top) top = bgl_w[i].prio;
    bgl_top_wait = top; g_bgl_contended = bgl_nw > 0;
}
/* under bgl_mu, by the releasing thread: the waiter that runs next. Decided once, here,
 * and handed over directly (waiters deciding for themselves with a time-dependent
 * rule could all decide "not me" and sleep with the lock free). */
static int bgl_grant = -1;
static int bgl_pick(uint64_t now) {
    int64_t best = INT64_MIN; int bi = -1;
    for (int i = 0; i < BGL_MAXW; i++) {
        if (!bgl_w[i].used || i == bgl_grant) continue;
        int64_t p = now - bgl_w[i].since > 50000 ? 1000 : bgl_w[i].prio;   /* starved: boost */
        int64_t key = p * (1ll << 40) - (int64_t)bgl_w[i].seq;
        if (key > best) { best = key; bi = i; }
    }
    return bi;
}
void bgl_acquire(void) {
    if (!bgl_enabled() || bgl_held) { bgl_held++; return; }
    int prio = bgl_my_prio();
    pthread_mutex_lock(&bgl_mu);
    uint64_t w0 = 0;
    if (!bgl_busy) bgl_busy = 1;                 /* free: nobody can be waiting */
    else {
        int me = 0; while (bgl_w[me].used) if (++me == BGL_MAXW) rt_fatal("single-core lock: too many threads");
        w0 = bgl_now_us();
        bgl_w[me].used = 1; bgl_w[me].prio = prio; bgl_w[me].seq = bgl_seq++; bgl_w[me].since = w0;
        bgl_nw++; bgl_recount();
        while (bgl_grant != me) pthread_cond_wait(&bgl_cv, &bgl_mu);
        bgl_grant = -1; bgl_w[me].used = 0; bgl_nw--; bgl_recount();
    }
    bgl_grants++; bgl_owner_prio = prio;
    pthread_mutex_unlock(&bgl_mu);
    if (w0) stall_add("(lock wait)", bgl_now_us() - w0);
    bgl_held = 1; bgl_since = bgl_now_us(); bgl_owner = (long)(uintptr_t)pthread_self(); bgl_owner_call = tls_last;
}
void bgl_release(void) {
    if (!bgl_enabled() || bgl_held > 1) { bgl_held--; return; }
    if (bgl_held <= 0) return;
    bgl_held = 0; bgl_owner = 0;
    pthread_mutex_lock(&bgl_mu);
    if (bgl_nw) { bgl_grant = bgl_pick(bgl_now_us()); pthread_cond_broadcast(&bgl_cv); }   /* stays busy: handed over */
    else bgl_busy = 0;
    pthread_mutex_unlock(&bgl_mu);
}
/* drop the lock around a blocking host call; returns the nesting depth to restore */
int bgl_drop(void) {
    int d = bgl_held;
    if (!bgl_enabled() || d <= 0) return 0;
    bgl_held = 1; bgl_release();
    return d;
}
void bgl_restore(int d) { if (d > 0) { bgl_acquire(); bgl_held = d; } }
/* hand over if a higher-priority thread waits, or after a 4 ms slice if anyone does */
static inline int bgl_should_yield(void) {
    if (bgl_top_wait > bgl_owner_prio) return 1;
    return bgl_now_us() - bgl_since >= 4000;
}
void bgl_preempt(void) {             /* called from lifted loops while contended */
    if (!bgl_enabled() || bgl_held != 1 || !g_bgl_contended) return;
    if (!bgl_should_yield()) return;
    stall_add("(handed over in a loop)", 0);
    bgl_release(); bgl_acquire();
}
static void bgl_maybe_yield(void) {
    if (!bgl_enabled() || bgl_held != 1 || !g_bgl_contended) return;
    if (!bgl_should_yield()) return;
    stall_add("(handed over)", 0);
    bgl_release(); bgl_acquire();
}
static int thunk_may_block(const char *n) {
    static const char *const pat[] = { "Wait", "Sleep", "GetMessage", "ExitThread",
        "recv", "select", "accept", "connect", "BufferSwap", "pageflip", "MessageBox", "SuspendThread",
        "ReadFile", "WriteFile", "CreateFileA", NULL };   /* file I/O: slow on OPFS */
    for (int i = 0; pat[i]; i++) if (strstr(n, pat[i])) return 1;
    return 0;
}

uint32_t thunk_new(const char *name, hostfn fn, int nargs) {
    pthread_mutex_lock(&thunk_mu);
    for (int i = 0; i < nthunks; i++)
        if (thunks[i].fn == fn && !strcmp(thunks[i].name, name)) {
            pthread_mutex_unlock(&thunk_mu); return THUNK_BASE + i * THUNK_STRIDE;
        }
    if (nthunks == MAX_THUNKS) rt_fatal("out of thunks");
    int i = nthunks++;
    thunks[i] = (Thunk){strdup(name), fn, nargs, thunk_may_block(name)};
    pthread_mutex_unlock(&thunk_mu);
    return THUNK_BASE + i * THUNK_STRIDE;
}
const char *thunk_name(uint32_t addr) {
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return (addr >= THUNK_BASE && i < (uint32_t)nthunks) ? thunks[i].name : NULL;
}

/* ------------------------------------------------------------- dispatch */
typedef struct { uint32_t addr; gfn fn; } FuncEnt;
extern const struct { const char *name; const FuncEnt *tab; const size_t *len; uint32_t base; } g_lifted[];

static FILE *missing_fp;
static pthread_mutex_t miss_mu = PTHREAD_MUTEX_INITIALIZER;

static gfn func_lookup(uint32_t a) {
    for (int k = 0; g_lifted[k].name; k++) {
        const FuncEnt *t = g_lifted[k].tab; size_t n = *g_lifted[k].len;
        if (!n || a < t[0].addr || a > t[n - 1].addr) continue;
        size_t lo = 0, hi = n;
        while (lo < hi) {
            size_t m = (lo + hi) / 2;
            if (t[m].addr < a) lo = m + 1; else hi = m;
        }
        if (lo < n && t[lo].addr == a) return t[lo].fn;
    }
    return NULL;
}
/* returns the base the module was lifted at (0 if not lifted) */
uint32_t lifted_base(const char *dll) {
    for (int k = 0; g_lifted[k].name; k++) if (!strcasecmp(g_lifted[k].name, dll)) return g_lifted[k].base;
    return 0;
}

/* ---------------------------------------------------- stall profiler
 * Always on and cheap: time per (API, thread) and lock waits since the last
 * frame. A frame that took > 300 ms logs where the time went (browser stalls). */
typedef struct { const char *name; uint32_t tid; uint32_t ep; uint64_t us, n; } StallEnt;
#define NSTALL 128
static StallEnt stall_tab[NSTALL];
static volatile uint32_t stall_ep = 1;
static uint64_t stall_frame_us;
volatile uint64_t g_io_read_bytes, g_io_reads;
static void stall_add(const char *name, uint64_t us) {
    uint32_t ep = stall_ep, tid = tls_tid;
    unsigned h = ((unsigned)(uintptr_t)name >> 3) * 31u + tid;
    for (int k = 0; k < 8; k++) {
        StallEnt *e = &stall_tab[(h + k) % NSTALL];
        if (e->ep != ep) { e->ep = ep; e->name = name; e->tid = tid; e->us = 0; e->n = 0; }
        else if (e->name != name || e->tid != tid) continue;
        e->us += us; e->n++; return;
    }
}
void rt_stall_frame(void) {       /* called at every grBufferSwap */
    uint64_t now = bgl_now_us();
    static uint64_t rd0, rn0;
    if (stall_frame_us && now - stall_frame_us > 300000) {
        StallEnt top[12]; int nt = 0;
        for (int i = 0; i < NSTALL; i++) {
            StallEnt e = stall_tab[i];
            if (e.ep != stall_ep || (e.us < 20000 && e.n < 50)) continue;
            int j;
            if (nt < 12) j = nt++;
            else if (top[11].us >= e.us) continue;
            else j = 11;
            top[j] = e;
            while (j > 0 && top[j].us > top[j - 1].us) { StallEnt x = top[j]; top[j] = top[j - 1]; top[j - 1] = x; j--; }
        }
        char buf[1400]; int o = 0;
        for (int i = 0; i < nt && o < (int)sizeof buf - 120; i++)
            o += snprintf(buf + o, sizeof buf - o, " | t%x %s %llums/%llu", top[i].tid, top[i].name,
                          (unsigned long long)(top[i].us / 1000), (unsigned long long)top[i].n);
        rt_log("stall: frame took %llu ms, read %llu KB in %llu reads%s", (unsigned long long)((now - stall_frame_us) / 1000),
               (unsigned long long)((g_io_read_bytes - rd0) >> 10), (unsigned long long)(g_io_reads - rn0), buf);
    }
    rd0 = g_io_read_bytes; rn0 = g_io_reads;
    stall_ep++; stall_frame_us = bgl_now_us();
}

void call_indirect(CPU *c, uint32_t t) {
    if (t >= THUNK_BASE && t < THUNK_BASE + MAX_THUNKS * THUNK_STRIDE) {
        Thunk *th = &thunks[(t - THUNK_BASE) / THUNK_STRIDE];
        uint32_t args[16];
        int n = th->nargs < 0 ? 16 : th->nargs;
        for (int i = 0; i < n && i < 16; i++) args[i] = RD32(c->r[ESP] + 4 + 4 * i);
        if (rt_verbose > 1) rt_log("call %s", th->name);
        const char *prev = tls_thunk_name; tls_thunk_name = th->name;
        uint32_t prev_ret = tls_ret; tls_ret = RD32(c->r[ESP]); tls_last = th->name; tls_calls++;
        uint32_t ret;
        uint64_t t0 = bgl_now_us();
        if (th->blocks) { int d = bgl_drop(); ret = th->fn(c, args); bgl_restore(d); }
        else { bgl_maybe_yield(); ret = th->fn(c, args); }
        stall_add(th->name, bgl_now_us() - t0);
        tls_thunk_name = prev; tls_ret = prev_ret;
        c->r[EAX] = ret;
        c->r[ESP] += 4 + (th->nargs > 0 ? 4 * th->nargs : 0);
        return;
    }
    if (t == RET_SENTINEL) rt_fatal("called return sentinel");
    gfn f = func_lookup(t);
    if (f) { f(c); return; }
    pthread_mutex_lock(&miss_mu);
    if (missing_fp) { fprintf(missing_fp, "%08x\n", t); fflush(missing_fp); }
    pthread_mutex_unlock(&miss_mu);
    guest_backtrace(c);
    rt_fatal("call to unlifted code %#x (ret addr %#x) - added to build/missing.txt, re-run lift with --seeds",
             t, RD32(c->r[ESP]));
}

/* ------------------------------------------------------------- tracing */
volatile int g_trace_any;
static uint32_t trace_set[64]; static int ntrace;
static int traced(uint32_t fn) { for (int i = 0; i < ntrace; i++) if (trace_set[i] == fn) return 1; return 0; }
static void fmt_arg(uint32_t v, char *out, size_t n) {
    /* show short strings for pointers to printable text */
    if (v >= 0x400000 && v < 0xFE000000u && G[v] >= 32 && G[v] < 127 && G[v + 1] >= 32 && G[v + 1] < 127)
        snprintf(out, n, "%#x\"%.40s\"", v, (char *)(G + v));
    else snprintf(out, n, "%#x", v);
}
void trace_enter(CPU *c, uint32_t fn) {
    if (!traced(fn)) return;
    char a[4][64];
    fmt_arg(c->r[EAX], a[0], 64); fmt_arg(c->r[EDX], a[1], 64); fmt_arg(c->r[EBX], a[2], 64); fmt_arg(c->r[ECX], a[3], 64);
    rt_log("-> %08x(eax=%s edx=%s ebx=%s ecx=%s) [ret %08x]", fn, a[0], a[1], a[2], a[3], RD32(c->r[ESP]));
    if (getenv("RT_PEEK")) {           /* RT_PEEK=hexaddr:len : dump guest memory at each traced entry */
        uint32_t pa = 0, pl = 32; sscanf(getenv("RT_PEEK"), "%x:%u", &pa, &pl); if (pl > 256) pl = 256;
        char h[1024]; int k = 0;
        for (uint32_t i = 0; i < pl; i++) k += snprintf(h + k, sizeof h - k, "%02x%s", G[pa + i], (i & 3) == 3 ? " " : "");
        rt_log("   [%#x] %s", pa, h);
    }
    if (getenv("RT_TRACE_DUMP") && c->r[EAX] >= 0x10000) {
        char h[128]; int k = 0;
        for (int i = 0; i < 32; i++) k += snprintf(h + k, sizeof h - k, "%02x%s", G[c->r[EAX] + i], (i & 3) == 3 ? " " : "");
        rt_log("   [eax] %s", h);
    }
}
void trace_leave(CPU *c, uint32_t fn) {
    if (traced(fn)) rt_log("<- %08x = %#x (ebx=%#x esi=%#x edi=%#x ebp=%#x esp=%#x)", fn, c->r[EAX], c->r[EBX], c->r[ESI], c->r[EDI], c->r[EBP], c->r[ESP]);
}
static void trace_init(void) {
    const char *t = getenv("RT_TRACE"); if (!t) return;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", t);
    for (char *p = strtok(buf, ","); p && ntrace < 64; p = strtok(NULL, ",")) trace_set[ntrace++] = (uint32_t)strtoul(p, NULL, 16);
    g_trace_any = ntrace > 0;
}

/* Heuristic backtrace: stack words that point just after a call instruction. */
void guest_backtrace(CPU *c) {
    char buf[512]; int k = snprintf(buf, sizeof buf, "guest stack:");
    int found = 0;
    for (uint32_t sp = c->r[ESP]; sp < c->r[ESP] + 0x2000 && found < 14; sp += 4) {
        uint32_t v = RD32(sp);
        if (!(v > CODE_LO && v < HEAP_LO)) continue;
        /* was the previous instruction a call? E8 rel32 (5 bytes) or FF /2 (2..7 bytes) */
        int is_call = RD8(v - 5) == 0xE8;
        for (int n = 2; n <= 7 && !is_call; n++) is_call = RD8(v - n) == 0xFF && ((RD8(v - n + 1) >> 3) & 7) == 2;
        if (!is_call) continue;
        k += snprintf(buf + k, sizeof buf - k, " %08x", v); found++;
    }
    rt_log("%s", buf);
}

uint32_t guest_call(CPU *c, uint32_t fn, int n, ...) {
    va_list ap; va_start(ap, n);
    uint32_t args[16];
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t sp = c->r[ESP];
    for (int i = n - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(RET_SENTINEL);
    bgl_acquire();                      /* host threads (timers, new threads) enter guest code here */
    call_indirect(c, fn);
    bgl_release();
    c->r[ESP] = sp;
    return c->r[EAX];
}

/* ----------------------------------------------------------- CPU / TEB */

static void cpu_init(CPU *c, uint32_t stack_top) {
    memset(c, 0, sizeof *c);
    c->r[ESP] = stack_top;
    c->fcw = 0x27F;
    c->fs_base = (galloc(0x2000) + 0xFFF) & ~0xFFFu;     /* one TEB page per thread, from the guest heap */
    WR32(c->fs_base + 0x00, 0xFFFFFFFFu);  /* SEH chain end */
    WR32(c->fs_base + 0x04, stack_top);
    WR32(c->fs_base + 0x08, stack_top - 0x100000);
    WR32(c->fs_base + 0x18, c->fs_base);   /* self */
    c->seg[0] = c->seg[2] = c->seg[3] = 0x23; c->seg[1] = 0x1B; c->seg[4] = 0x3B;
}
CPU *cpu_new_thread(uint32_t stack_size) {
    if (stack_size < 0x40000) stack_size = 0x40000;
    uint32_t base = galloc(stack_size);
    if (!base) rt_fatal("no memory for thread stack");
    CPU *c = calloc(1, sizeof *c);
    cpu_init(c, (base + stack_size - 64) & ~15u);
    return c;
}

/* ---------------------------------------------------------- CPU helpers */
double fround(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);       /* host default mode is round-to-nearest-even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
double RDF80(uint32_t a) {
    uint64_t m = RD64(a); uint16_t se = RD16(a + 8);
    int e = se & 0x7FFF, s = se >> 15;
    if (e == 0 && m == 0) return s ? -0.0 : 0.0;
    if (e == 0x7FFF) return (m << 1) ? NAN : (s ? -INFINITY : INFINITY);
    double v = ldexp((double)m, e - 16383 - 63);
    return s ? -v : v;
}
void WRF80(uint32_t a, double v) {
    uint64_t m = 0; uint16_t se = 0;
    if (signbit(v)) { se = 0x8000; v = -v; }
    if (v == 0) {
    } else if (isinf(v)) { se |= 0x7FFF; m = 1ull << 63; }
    else if (isnan(v)) { se |= 0x7FFF; m = 3ull << 62; }
    else {
        int e; double f = frexp(v, &e);      /* v = f * 2^e, f in [0.5,1) */
        m = (uint64_t)ldexp(f, 64); se |= (uint16_t)(e - 1 + 16383);
    }
    WR64(a, m); WR16(a + 8, se);
}
void x86_trap(CPU *c, uint32_t addr, const char *what) {
    guest_backtrace(c);
    rt_fatal("trap at %#x: %s (eax=%x ecx=%x edx=%x ebx=%x esp=%x ebp=%x esi=%x edi=%x)",
             addr, what, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
}
void x86_int(CPU *c, uint32_t addr, uint32_t n) {
    if (n == 3) { rt_log("int3 at %#x ignored", addr); return; }
    x86_trap(c, addr, "software interrupt");
}
void x86_cpuid(CPU *c) {
    /* Pentium II-class: FPU, TSC, CX8, CMOV, MMX. No SSE: the game predates it. */
    switch (c->r[EAX]) {
    case 0: c->r[EAX] = 1; c->r[EBX] = 0x756E6547; c->r[EDX] = 0x49656E69; c->r[ECX] = 0x6C65746E; break;
    case 1: c->r[EAX] = 0x652; c->r[EBX] = 0; c->r[ECX] = 0; c->r[EDX] = 0x0000A111; break; /* FPU TSC CX8 CMOV; no MMX so MMX paths stay off */
    default: c->r[EAX] = c->r[EBX] = c->r[ECX] = c->r[EDX] = 0;
    }
}
void x86_rdtsc(CPU *c) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 400000000ull + (uint64_t)ts.tv_nsec * 2 / 5; /* 400 MHz */
    c->r[EAX] = (uint32_t)t; c->r[EDX] = (uint32_t)(t >> 32);
}
void x86_fnsave(CPU *c, uint32_t a) {
    WR16(a, c->fcw); WR16(a + 4, fstsw(c));
    for (int i = 0; i < 8; i++) WRF80(a + 28 + 10 * i, ST(i));
    c->top = 0; c->fsw = 0; c->fcw = 0x37F;
}
void x86_frstor(CPU *c, uint32_t a) {
    c->fcw = RD16(a); uint16_t sw = RD16(a + 4);
    c->top = (sw >> 11) & 7; c->fsw = sw & ~0x3800;
    for (int i = 0; i < 8; i++) ST(i) = RDF80(a + 28 + 10 * i);
}

/* --------------------------------------------------------------- handles */
#define MAX_HANDLES 4096
static struct { int type; void *obj; } handles[MAX_HANDLES];
static pthread_mutex_t handle_mu = PTHREAD_MUTEX_INITIALIZER;
#define HBASE 0x1000u
uint32_t handle_new(int type, void *obj) {
    pthread_mutex_lock(&handle_mu);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i].type) {
            handles[i].type = type; handles[i].obj = obj;
            pthread_mutex_unlock(&handle_mu);
            return HBASE + i * 4;
        }
    pthread_mutex_unlock(&handle_mu);
    rt_fatal("out of handles");
}
static int hidx(uint32_t h) {
    if (h < HBASE || (h - HBASE) % 4) return -1;
    uint32_t i = (h - HBASE) / 4;
    return i < MAX_HANDLES ? (int)i : -1;
}
void *handle_get(uint32_t h, int type) {
    int i = hidx(h);
    if (i < 0 || handles[i].type != type) return NULL;
    return handles[i].obj;
}
int handle_type(uint32_t h) { int i = hidx(h); return i < 0 ? H_NONE : handles[i].type; }
void handle_close(uint32_t h) {
    int i = hidx(h);
    if (i >= 0) { pthread_mutex_lock(&handle_mu); handles[i].type = 0; handles[i].obj = NULL; pthread_mutex_unlock(&handle_mu); }
}

/* --------------------------------------------------------------- imports */
static const ImportDef *const all_imports[] = {
    imp_kernel32, imp_user32, imp_gdi32, imp_advapi32, imp_ole32,
    imp_shell32, imp_winmm, imp_wsock32, imp_dinput, imp_ddraw, imp_dsound, imp_extra,
};
static uint32_t unimpl(CPU *c, const uint32_t *a);

const ImportDef *import_find(const char *dll, const char *name) {
    for (size_t t = 0; t < sizeof all_imports / sizeof *all_imports; t++)
        for (const ImportDef *d = all_imports[t]; d->name; d++)
            if (!strcasecmp(d->dll, dll) && !strcmp(d->name, name)) return d;
    return NULL;
}
static uint32_t unimpl(CPU *c, const uint32_t *a) {
    (void)a; (void)c;
    return 0;
}
uint32_t import_thunk(const char *dll, const char *name) {
    char full[256];
    snprintf(full, sizeof full, "%s!%s", dll, name);
    const ImportDef *d = import_find(dll, name);
    if (!d) { rt_log("no definition for import %s (stdcall arg count unknown)", full); return thunk_new(full, unimpl, 0); }
    if (!d->fn) rt_log("import %s is a stub", full);
    return thunk_new(full, d->fn ? d->fn : unimpl, d->nargs);
}

/* ------------------------------------------------------------ PE loader */
static uint32_t rd32f(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16f(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

uint32_t g_image_base, g_image_size;
char g_exe_path[1024];

/* WSOCK32 exports by ordinal */
static const char *ordinal_name(const char *dll, uint32_t o) {
    static const struct { uint32_t o; const char *n; } ws[] = {
        {1,"accept"},{2,"bind"},{3,"closesocket"},{4,"connect"},{6,"getsockname"},{9,"htons"},
        {10,"inet_addr"},{11,"inet_ntoa"},{12,"ioctlsocket"},{13,"listen"},{15,"ntohs"},{16,"recv"},
        {17,"recvfrom"},{18,"select"},{19,"send"},{20,"sendto"},{21,"setsockopt"},{22,"shutdown"},
        {23,"socket"},{52,"gethostbyname"},{57,"gethostname"},{111,"WSAGetLastError"},
        {115,"WSAStartup"},{116,"WSACleanup"},{151,"__WSAFDIsSet"},
    };
    if (!strcasecmp(dll, "DSOUND.dll") && o == 1) return "DirectSoundCreate";
    if (strcasecmp(dll, "WSOCK32.dll")) return NULL;
    for (size_t i = 0; i < sizeof ws / sizeof *ws; i++) if (ws[i].o == o) return ws[i].n;
    return NULL;
}

uint32_t load_pe(const char *path, uint32_t *entry_out) { return load_pe_at(path, entry_out, 0); }
uint32_t load_pe_at(const char *path, uint32_t *entry_out, uint32_t want_base) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) rt_fatal("read %s", path);
    fclose(f);
    uint32_t pe = rd32f(buf + 0x3C);
    if (memcmp(buf + pe, "PE\0\0", 4)) rt_fatal("not a PE file");
    uint16_t nsec = rd16f(buf + pe + 6), optsz = rd16f(buf + pe + 20);
    const uint8_t *opt = buf + pe + 24;
    uint32_t pref = rd32f(opt + 28), size = rd32f(opt + 56);
    uint32_t base = want_base ? want_base : pref;
    if (!g_image_base) { g_image_base = base; g_image_size = size; }
    if (entry_out) *entry_out = rd32f(opt + 16) ? base + rd32f(opt + 16) : 0;
    uint32_t hdrsz = rd32f(opt + 60);
    memcpy(G + base, buf, hdrsz);
    const uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsz = rd32f(sec + 8), va = rd32f(sec + 12), rsz = rd32f(sec + 16), roff = rd32f(sec + 20);
        /* copy only bytes that exist in the file; the rest of the section
         * (e.g. .bss, which may carry a bogus raw size) stays zero */
        uint32_t cnt = (vsz && vsz < rsz) ? vsz : rsz;
        if (!roff || roff >= (uint32_t)n) cnt = 0;
        else if (cnt > (uint32_t)n - roff) cnt = (uint32_t)n - roff;
        if (cnt) memcpy(G + base + va, buf + roff, cnt);
    }
    if (base != pref) {                       /* apply base relocations */
        uint32_t rel = rd32f(opt + 136), relsz = rd32f(opt + 140), delta = base - pref, n = 0;
        for (uint32_t b = base + rel; b < base + rel + relsz && RD32(b + 4); b += RD32(b + 4)) {
            uint32_t page = RD32(b), cnt = (RD32(b + 4) - 8) / 2;
            for (uint32_t i = 0; i < cnt; i++) {
                uint16_t e = RD16(b + 8 + 2 * i);
                if ((e >> 12) == 3) { uint32_t at = base + page + (e & 0xFFF); WR32(at, RD32(at) + delta); n++; }
            }
        }
        WR32(base + RD32(base + 0x3C) + 24 + 28, base);
        rt_log("rebased %s %#x -> %#x (%u fixups)", path, pref, base, n);
    }
    /* imports: fill IAT with thunk addresses */
    uint32_t imp_rva = rd32f(opt + 104);
    for (uint32_t d = base + imp_rva; imp_rva && RD32(d + 12); d += 20) {
        const char *dll = HSTR(base + RD32(d + 12));
        uint32_t ilt = RD32(d) ? RD32(d) : RD32(d + 16), iat = RD32(d + 16);
        for (uint32_t k = 0;; k++) {
            uint32_t e = RD32(base + ilt + 4 * k);
            if (!e) break;
            char nm[64];
            if (e & 0x80000000u) {
                const char *on = ordinal_name(dll, e & 0xFFFF);
                if (on) snprintf(nm, sizeof nm, "%s", on); else snprintf(nm, sizeof nm, "#%u", e & 0xFFFF);
            }
            else snprintf(nm, sizeof nm, "%s", HSTR(base + e + 2));
            char dl[64]; snprintf(dl, sizeof dl, "%s", dll);
            for (char *p = dl; *p; p++) *p = (char)toupper(*p);
            WR32(base + iat + 4 * k, import_thunk(dl, nm));
        }
    }
    free(buf);
    rt_log("loaded %s at %#x (%#x bytes)", path, base, size);
    { extern void glide_patch_driver(uint32_t, uint32_t); const char *s = strrchr(path, '/');
      if (s && !strcasecmp(s + 1, "voodoo2a.dll")) glide_patch_driver(base, size); }
    return base;
}

uint32_t pe_export(uint32_t base, const char *name) {
    uint32_t pe = base + RD32(base + 0x3C), ed = RD32(pe + 24 + 96);
    if (!ed) return 0;
    ed += base;
    uint32_t n = RD32(ed + 24), names = base + RD32(ed + 32), ords = base + RD32(ed + 36), funcs = base + RD32(ed + 28);
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(HSTR(base + RD32(names + 4 * i)), name))
            return base + RD32(funcs + 4 * RD16(ords + 2 * i));
    return 0;
}

/* ------------------------------------------------------------------ main */
#include <ctype.h>
void win32_init(void);
int display_init(void);   /* 1 if a window is open: call display_loop() on this thread */
void display_loop(void);

#ifdef __EMSCRIPTEN__
#include <emscripten/wasmfs.h>
/* The page copies the player's NFS4 folder into the Origin Private File System
 * (navigator.storage) once; WasmFS exposes it here as /opfs. */
static void web_mount_storage(void) {
    backend_t opfs = wasmfs_create_opfs_backend();
    if (wasmfs_create_directory("/opfs", 0777, opfs)) rt_fatal("cannot mount OPFS");
}
#endif

static uint32_t g_entry_point;
static void *game_thread(void *unused) {
    (void)unused;
    CPU *c = calloc(1, sizeof *c);
    cpu_init(c, STACK_MAIN_TOP);
    tls_cpu = c;
    rt_watch_thread(0);
    { extern void gpu_start_watchdog(void); gpu_start_watchdog(); }
    PUSH32(RET_SENTINEL);
    rt_log("entering guest entry point %#x", g_entry_point);
    bgl_acquire();
    call_indirect(c, g_entry_point);
    rt_log("entry point returned eax=%#x", c->r[EAX]);
    exit((int)c->r[EAX]);
}

#ifndef __EMSCRIPTEN__
/* RT_PROF=file: sample the native PC of every thread ~1 kHz (CPU time) and append
 * raw addresses to <file>; tools/prof.py maps them to functions/modules. */
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
static FILE *prof_fp; static double prof_t0, prof_start;
static double prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void prof_sig(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    if (prof_now() - prof_t0 < prof_start) return;
    uintptr_t pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
    fwrite(&pc, sizeof pc, 1, prof_fp);
}
static void prof_init(void) {
    const char *f = getenv("RT_PROF"); if (!f) return;
    prof_fp = fopen(f, "wb"); if (!prof_fp) return;
    { int main(int, char **); uintptr_t m = (uintptr_t)&main; fwrite(&m, sizeof m, 1, prof_fp); }   /* header: &main */
    { char mp[512]; snprintf(mp, sizeof mp, "%s.maps", f); FILE *o = fopen(mp, "w"), *i = fopen("/proc/self/maps", "r");
      if (o && i) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, i)) > 0) fwrite(b, 1, n, o); }
      if (o) fclose(o); if (i) fclose(i); }
    prof_t0 = prof_now(); if (getenv("RT_PROF_START")) prof_start = atof(getenv("RT_PROF_START"));  /* seconds */
    struct sigaction sa = {0}; sa.sa_sigaction = prof_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, NULL);
    atexit((void (*)(void))fflush);
}
#endif

int main(int argc, char **argv) {
    const char *dir = getenv("NFS4_DIR");
    if (argc > 1) dir = argv[1];
    /* further arguments KEY=VALUE are settings (the page passes e.g. RT_GLIDE_MODES this way) */
    for (int i = 2; i < argc; i++) { char *eq = strchr(argv[i], '='); if (eq && eq != argv[i]) { *eq = 0; setenv(argv[i], eq + 1, 1); *eq = '='; } }
#ifdef __EMSCRIPTEN__
    web_mount_storage();
    if (!dir) dir = "/opfs/nfs4";
#endif
    if (!dir) { fprintf(stderr, "usage: %s <path to NFS4 folder>\n", argv[0]); return 1; }
    if (getenv("RT_VERBOSE")) rt_verbose = atoi(getenv("RT_VERBOSE"));
    if (getenv("RT_TICK_MS")) rt_tick_ms = atof(getenv("RT_TICK_MS"));
    snprintf(g_game_dir, sizeof g_game_dir, "%s", dir);
    trace_init();
#ifndef __EMSCRIPTEN__
    if (getenv("RT_ALARM")) alarm((unsigned)atoi(getenv("RT_ALARM")));  /* debugging: stop under gdb */
    prof_init();
#endif
#ifdef __EMSCRIPTEN__
    G = aligned_alloc(0x10000, GUEST_SIZE);               /* wasm heap; zeroed below */
    if (!G) rt_fatal("cannot allocate %u MiB guest memory", GUEST_SIZE >> 20);
    memset(G, 0, GUEST_SIZE);
#else
    /* 4 GiB reservation: stray guest pointers fault instead of hitting host memory */
    G = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (G == MAP_FAILED) rt_fatal("cannot reserve guest address space");
    /* RT_GUARD=1: fault on guest accesses past GUEST_SIZE, like the browser build does (its wasm
     * memory ends there); natively they would silently read zeros */
    if (getenv("RT_GUARD") && atoi(getenv("RT_GUARD"))) mprotect(G + GUEST_SIZE, 0x100000000ull - GUEST_SIZE, PROT_NONE);
#endif
    missing_fp = fopen("build/missing.txt", "a");
    snprintf(g_exe_path, sizeof g_exe_path, "%s/nfs4.exe", dir);
    uint32_t entry;
    load_pe(g_exe_path, &entry);
    win32_init();
    g_entry_point = entry;
    if (display_init()) {
        /* SDL wants the window and its events on the main thread:
         * the game runs on its own thread with a large native stack */
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 256u << 20);
        pthread_t t;
        if (pthread_create(&t, &at, game_thread, NULL)) rt_fatal("cannot start game thread");
        display_loop();                  /* never returns; exits the process on window close */
    }
    game_thread(NULL);
    return 0;
}

void bgl_dump(void) { rt_log("single-core lock: %llu grants, %d waiting (top priority %d), owner thread %#lx priority %d (acquired after %s)", (unsigned long long)bgl_grants, bgl_nw, bgl_top_wait, bgl_owner, bgl_owner_prio, bgl_owner_call ? bgl_owner_call : "-"); }
