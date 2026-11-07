/* USER32 / GDI32 / ADVAPI32 / OLE32 / SHELL32 / WINMM / DINPUT. */
#define _GNU_SOURCE
#include "rt.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* --------------------------------------------------------------- input */
/* Host (SDL window natively, the browser on web) writes key state here. */
volatile uint8_t g_keys[256];
volatile int32_t g_mouse_x, g_mouse_y;
int g_screen_w = 640, g_screen_h = 480;
uint32_t g_main_hwnd;            /* first top-level window: input messages go here */

/* ------------------------------------------------------------- windows */
typedef struct { char name[64]; uint32_t proc; } WClass;
static WClass classes[32]; static int nclasses;
typedef struct { uint32_t proc; uint32_t longs[16]; int x, y, w, h; } Window;

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_QUIT 0x0012
#define WM_ACTIVATEAPP 0x001C
#define WM_SHOWWINDOW 0x0018

typedef struct { uint32_t hwnd, msg, wp, lp; } Msg;
#define QCAP 1024
static Msg q[QCAP]; static int qh, qt;
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;

void msg_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&q_mu);
    if ((qt + 1) % QCAP != qh) { q[qt] = (Msg){hwnd, msg, wp, lp}; qt = (qt + 1) % QCAP; }
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mu);
}

static uint32_t send_msg(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Window *w = handle_get(hwnd, H_WINDOW);
    if (!w || !w->proc) return 0;
    return guest_call(c, w->proc, 4, hwnd, msg, wp, lp);
}

static uint32_t u_RegisterClassA(CPU *c, const uint32_t *a) {
    uint32_t wc = A(0);
    WClass *k = &classes[nclasses++];
    k->proc = RD32(wc + 4);
    uint32_t nm = RD32(wc + 36);
    if (nm >= 0x10000) snprintf(k->name, sizeof k->name, "%s", HSTR(nm));
    else snprintf(k->name, sizeof k->name, "#%u", nm);
    rt_log("RegisterClassA(%s) wndproc=%#x", k->name, k->proc);
    return 0xC000u + (uint32_t)nclasses;
}
static uint32_t u_CreateWindowExA(CPU *c, const uint32_t *a) {
    const char *cls = A(1) >= 0x10000 ? HSTR(A(1)) : "";
    Window *w = calloc(1, sizeof *w);
    for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i].name, cls)) w->proc = classes[i].proc;
    w->x = (int)A(4); w->y = (int)A(5); w->w = g_screen_w; w->h = g_screen_h;
    uint32_t h = handle_new(H_WINDOW, w);
    if (!g_main_hwnd) g_main_hwnd = h;
    rt_log("CreateWindowExA(class=%s, title=%s) -> %#x", cls, A(2) ? HSTR(A(2)) : "", h);
    uint32_t cs = galloc(48);
    WR32(cs, A(11)); WR32(cs + 4, A(10)); WR32(cs + 32, A(2)); WR32(cs + 36, A(1));
    send_msg(c, h, WM_CREATE, 0, cs);
    return h;
}
static uint32_t u_ShowWindow(CPU *c, const uint32_t *a) {
    msg_post(A(0), WM_SHOWWINDOW, 1, 0);
    msg_post(A(0), WM_ACTIVATEAPP, 1, 0);
    msg_post(A(0), WM_ACTIVATE, 1, 0);
    msg_post(A(0), WM_SETFOCUS, 0, 0);
    return 0;
}
static uint32_t u_DestroyWindow(CPU *c, const uint32_t *a) { send_msg(c, A(0), WM_DESTROY, 0, 0); return 1; }
static uint32_t u_DefWindowProcA(CPU *c, const uint32_t *a) { return 0; }
static uint32_t u_GetMessageA(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&q_mu);
    while (qh == qt) pthread_cond_wait(&q_cv, &q_mu);
    Msg m = q[qh]; qh = (qh + 1) % QCAP;
    pthread_mutex_unlock(&q_mu);
    uint32_t p = A(0);
    WR32(p, m.hwnd); WR32(p + 4, m.msg); WR32(p + 8, m.wp); WR32(p + 12, m.lp);
    WR32(p + 16, 0); WR32(p + 20, (uint32_t)g_mouse_x); WR32(p + 24, (uint32_t)g_mouse_y);
    if (rt_verbose > 1) rt_log("GetMessage %#x", m.msg);
    return m.msg != WM_QUIT;
}
static uint32_t u_TranslateMessage(CPU *c, const uint32_t *a) { return 0; }
static uint32_t u_DispatchMessageA(CPU *c, const uint32_t *a) {
    uint32_t p = A(0);
    return send_msg(c, RD32(p), RD32(p + 4), RD32(p + 8), RD32(p + 12));
}
static uint32_t u_PostMessageA(CPU *c, const uint32_t *a) { msg_post(A(0), A(1), A(2), A(3)); return 1; }
static uint32_t u_PostThreadMessageA(CPU *c, const uint32_t *a) { msg_post(0, A(1), A(2), A(3)); return 1; }
static uint32_t u_SendNotifyMessageA(CPU *c, const uint32_t *a) { msg_post(A(0), A(1), A(2), A(3)); return 1; }
static uint32_t u_PostQuitMessage(CPU *c, const uint32_t *a) { msg_post(0, WM_QUIT, A(0), 0); return 0; }
static uint32_t u_GetWindowLongA(CPU *c, const uint32_t *a) {
    Window *w = handle_get(A(0), H_WINDOW); if (!w) return 0;
    int i = (int)A(1);
    if (i == -4) return w->proc;
    return i >= 0 && i < 64 ? w->longs[i / 4] : 0;
}
static uint32_t u_GetClientRect(CPU *c, const uint32_t *a) {
    WR32(A(1), 0); WR32(A(1) + 4, 0); WR32(A(1) + 8, (uint32_t)g_screen_w); WR32(A(1) + 12, (uint32_t)g_screen_h);
    return 1;
}
static uint32_t u_AdjustWindowRect(CPU *c, const uint32_t *a) { return 1; }
static uint32_t u_ClientToScreen(CPU *c, const uint32_t *a) { return 1; }
static uint32_t u_GetSystemMetrics(CPU *c, const uint32_t *a) {
    switch (A(0)) {
    case 0: return (uint32_t)g_screen_w; case 1: return (uint32_t)g_screen_h;
    case 0x3f: return 1;   /* SM_NETWORK: a network is present (the game hides network play otherwise) */
    default: return 0;
    }
}
static uint32_t u_GetAsyncKeyState(CPU *c, const uint32_t *a) { return g_keys[A(0) & 0xFF] ? 0x8000u : 0; }
static uint32_t u_GetKeyState(CPU *c, const uint32_t *a) { return g_keys[A(0) & 0xFF] ? 0xFF80u : 0; }
static uint32_t u_GetKeyboardType(CPU *c, const uint32_t *a) { return A(0) == 0 ? 4 : A(0) == 2 ? 12 : 0; }
static uint32_t u_MessageBoxA(CPU *c, const uint32_t *a) {
    const char *cap = A(2) ? HSTR(A(2)) : "", *txt = A(1) ? HSTR(A(1)) : "";
    fprintf(stderr, "[rt] MessageBoxA: [%s] %s\n", cap, txt);   /* always: usually the game's last words */
    guest_backtrace(c);
#ifdef __EMSCRIPTEN__
    char *m = malloc(strlen(cap) + strlen(txt) + 4);
    sprintf(m, "%s %s", cap, txt);
    MAIN_THREAD_ASYNC_EM_ASM({ const t = UTF8ToString($0); _free($0); if (Module.onGameMessage) Module.onGameMessage(t); }, m);
#endif
    return 1; /* IDOK */
}
static uint32_t u_CharUpperA(CPU *c, const uint32_t *a) {
    if (A(0) < 0x10000) return (uint32_t)toupper((int)A(0));
    for (char *s = HSTR(A(0)); *s; s++) *s = (char)toupper((unsigned char)*s);
    return A(0);
}
static uint32_t u_CharUpperBuffA(CPU *c, const uint32_t *a) {
    char *s = HSTR(A(0)); for (uint32_t i = 0; i < A(1); i++) s[i] = (char)toupper((unsigned char)s[i]);
    return A(1);
}
static uint32_t u_ShowCursor(CPU *c, const uint32_t *a) { return A(0) ? 1 : 0xFFFFFFFFu; }
static uint32_t u_one(CPU *c, const uint32_t *a) { return 1; }
static uint32_t u_zero(CPU *c, const uint32_t *a) { return 0; }
static uint32_t u_OffsetRect(CPU *c, const uint32_t *a) {
    WR32(A(0), RD32(A(0)) + A(1)); WR32(A(0) + 8, RD32(A(0) + 8) + A(1));
    WR32(A(0) + 4, RD32(A(0) + 4) + A(2)); WR32(A(0) + 12, RD32(A(0) + 12) + A(2)); return 1;
}
static uint32_t u_GetDesktopWindow(CPU *c, const uint32_t *a) { return 0x10010u; }
static uint32_t u_GetWindowDC(CPU *c, const uint32_t *a) { return 0x20020u; }
static uint32_t u_BeginPaint(CPU *c, const uint32_t *a) { memset(G + A(1), 0, 64); return 0x20020u; }
static uint32_t u_LoadIconA(CPU *c, const uint32_t *a) { return 0x30030u; }
static uint32_t u_SystemParametersInfoA(CPU *c, const uint32_t *a) { return 1; }

/* wsprintfA is cdecl with varargs: implement %d %i %u %x %X %s %c %ld %lu %02d etc. */
static uint32_t u_wsprintfA(CPU *c, const uint32_t *a) {
    char *out = HSTR(A(0)); const char *f = HSTR(A(1));
    int ai = 2; char *o = out;
    while (*f) {
        if (*f != '%') { *o++ = *f++; continue; }
        char spec[32]; int k = 0; spec[k++] = *f++;
        if (*f == '%') { *o++ = '%'; f++; continue; }
        while (*f && strchr("-+ #0123456789.", *f) && k < 28) spec[k++] = *f++;
        while (*f == 'l' || *f == 'h') f++;
        char t = *f++; spec[k++] = t; spec[k] = 0;
        uint32_t v = RD32(c->r[ESP] + 4 + 4 * ai++);
        if (t == 's') o += sprintf(o, spec, v ? HSTR(v) : "(null)");
        else o += sprintf(o, spec, v);
    }
    *o = 0;
    return (uint32_t)(o - out);
}

#define U(n, args) {"USER32.DLL", #n, args, u_##n}
#define US(n, args, fn) {"USER32.DLL", #n, args, fn}
const ImportDef imp_user32[] = {
    U(AdjustWindowRect, 3), U(BeginPaint, 2), US(CallNextHookEx, 4, u_zero), U(CharUpperA, 1),
    U(CharUpperBuffA, 2), U(ClientToScreen, 2), U(CreateWindowExA, 12), US(ReleaseDC, 2, u_one),
    US(DdeConnect, 4, u_zero), US(DdeCreateStringHandleA, 3, u_zero), US(DdeGetLastError, 1, u_zero),
    US(DdeInitializeA, 4, u_one), US(DdeQueryStringA, 5, u_zero), US(DdeUninitialize, 1, u_one),
    U(DefWindowProcA, 4), U(DestroyWindow, 1), U(DispatchMessageA, 1), US(EndPaint, 2, u_one),
    US(FindWindowA, 2, u_zero), U(GetAsyncKeyState, 1), U(GetClientRect, 2), U(GetDesktopWindow, 0),
    U(GetKeyState, 1), U(GetKeyboardType, 1), U(GetMessageA, 4), U(GetSystemMetrics, 1),
    U(GetWindowDC, 1), U(GetWindowLongA, 2), US(IsIconic, 1, u_zero), U(LoadIconA, 2), U(MessageBoxA, 4),
    U(PostMessageA, 4), U(PostQuitMessage, 1), U(PostThreadMessageA, 4), U(RegisterClassA, 1),
    U(SendNotifyMessageA, 4), US(SetActiveWindow, 1, u_zero), US(SetCursor, 1, u_zero),
    US(SetCursorPos, 2, u_one), US(SetForegroundWindow, 1, u_one), US(SetWindowPos, 7, u_one),
    US(SetWindowsHookExA, 4, u_one), U(ShowCursor, 1), U(ShowWindow, 2), US(SwapMouseButton, 1, u_zero),
    U(SystemParametersInfoA, 4), US(TranslateMessage, 1, u_TranslateMessage), US(UnhookWindowsHookEx, 1, u_one),
    US(OffsetRect, 3, u_OffsetRect), US(keybd_event, 4, u_zero), US(wsprintfA, -1, u_wsprintfA),
    {0},
};

/* ------------------------------------------------------------------ GDI32 */
static uint32_t g_GetDeviceCaps(CPU *c, const uint32_t *a) {
    switch (A(1)) {
    case 8: return (uint32_t)g_screen_w; case 10: return (uint32_t)g_screen_h;
    case 12: return 32; case 14: return 1; case 116: return 60; default: return 0;
    }
}
const ImportDef imp_gdi32[] = {
    {"GDI32.DLL", "GetDeviceCaps", 2, g_GetDeviceCaps},
    {"GDI32.DLL", "GetStockObject", 1, u_one},
    {0},
};

/* --------------------------------------------------------------- ADVAPI32 */
static uint32_t r_notfound(CPU *c, const uint32_t *a) { return 2; }
const ImportDef imp_advapi32[] = {
    {"ADVAPI32.DLL", "RegCloseKey", 1, u_zero},
    {"ADVAPI32.DLL", "RegFlushKey", 1, u_zero},
    {"ADVAPI32.DLL", "RegOpenKeyExA", 5, r_notfound},
    {"ADVAPI32.DLL", "RegQueryValueExA", 6, r_notfound},
    {0},
};

/* ---------------------------------------------------------- OLE32/SHELL32 */
static uint32_t o_CoCreateGuid(CPU *c, const uint32_t *a) {
    for (int i = 0; i < 16; i++) WR8(A(0) + i, (uint8_t)rand());
    return 0;
}
static uint32_t s_ShellExecuteA(CPU *c, const uint32_t *a) {
    rt_log("ShellExecuteA(%s) ignored", A(2) ? HSTR(A(2)) : ""); return 33;
}
const ImportDef imp_ole32[] = { {"OLE32.DLL", "CoCreateGuid", 1, o_CoCreateGuid}, {0} };
const ImportDef imp_shell32[] = { {"SHELL32.DLL", "ShellExecuteA", 6, s_ShellExecuteA}, {0} };

/* ------------------------------------------------------------------ WINMM */
static uint32_t ms_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static uint32_t w_timeGetTime(CPU *c, const uint32_t *a) { return ms_now(); }
static uint32_t w_timeGetDevCaps(CPU *c, const uint32_t *a) { WR32(A(0), 1); WR32(A(0) + 4, 1000000); return 0; }

/* All multimedia timers are serviced by one thread with one guest CPU, as on
 * Windows. Callbacks may re-arm (timeSetEvent) or kill timers re-entrantly. */
typedef struct { uint32_t delay, cb, user, flags, id; uint64_t due; int used; } MMTimer;
#define MAX_TIMERS 64
static MMTimer timers[MAX_TIMERS];
static uint32_t next_timer_id = 1;
static pthread_mutex_t timer_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cv;
static pthread_t timer_thread;
static int timer_started;

static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static void *timer_main(void *unused) {
    (void)unused;
    CPU *tc = cpu_new_thread(0x40000);
    tls_cpu = tc;
    rt_watch_thread(0xfff0);                 /* shows as the multimedia timer thread */
    pthread_mutex_lock(&timer_mu);
    for (;;) {
        int k = -1;
        for (int i = 0; i < MAX_TIMERS; i++) if (timers[i].used && (k < 0 || timers[i].due < timers[k].due)) k = i;
        if (k < 0) { pthread_cond_wait(&timer_cv, &timer_mu); continue; }
        uint64_t now = mono_ns();
        if (timers[k].due > now) {
            struct timespec dl; uint64_t d = timers[k].due;
            dl.tv_sec = (time_t)(d / 1000000000ull); dl.tv_nsec = (long)(d % 1000000000ull);
            pthread_cond_timedwait(&timer_cv, &timer_mu, &dl);
            continue;
        }
        MMTimer t = timers[k];
        if (t.flags & 1) timers[k].due += (uint64_t)t.delay * 1000000ull;   /* periodic */
        else timers[k].used = 0;                                            /* one-shot: free before callback */
        pthread_mutex_unlock(&timer_mu);
        if (rt_verbose > 1) rt_log("timer %u fires (cb=%#x)", t.id, t.cb);
        guest_call(tc, t.cb, 5, t.id, 0u, t.user, 0u, 0u);
        pthread_mutex_lock(&timer_mu);
    }
    return NULL;
}
static uint32_t w_timeSetEvent(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&timer_mu);
    if (!timer_started) {
        pthread_condattr_t at; pthread_condattr_init(&at); pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
        pthread_cond_init(&timer_cv, &at);
        pthread_create(&timer_thread, NULL, timer_main, NULL);
        timer_started = 1;
    }
    int k = -1;
    for (int i = 0; i < MAX_TIMERS; i++) if (!timers[i].used) { k = i; break; }
    if (k < 0) { pthread_mutex_unlock(&timer_mu); rt_log("timeSetEvent: out of timers"); return 0; }
    MMTimer *t = &timers[k];
    t->delay = A(0) ? A(0) : 1; t->cb = A(2); t->user = A(3); t->flags = A(4); t->used = 1;
    t->due = mono_ns() + (uint64_t)t->delay * 1000000ull;
    t->id = next_timer_id++; if (!next_timer_id) next_timer_id = 1;
    uint32_t id = t->id;
    pthread_cond_signal(&timer_cv);
    pthread_mutex_unlock(&timer_mu);
    if (rt_verbose > 1) rt_log("timeSetEvent(%ums, cb=%#x, %s) -> %u", A(0), A(2), (A(4) & 1) ? "periodic" : "oneshot", id);
    return id;
}
static uint32_t w_timeKillEvent(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&timer_mu);
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].used && timers[i].id == A(0)) { timers[i].used = 0; pthread_mutex_unlock(&timer_mu); return 0; }
    pthread_mutex_unlock(&timer_mu);
    return 97; /* MMSYSERR_INVALPARAM */
}
const ImportDef imp_winmm[] = {
    {"WINMM.DLL", "timeBeginPeriod", 1, u_zero}, {"WINMM.DLL", "timeEndPeriod", 1, u_zero},
    {"WINMM.DLL", "timeGetDevCaps", 2, w_timeGetDevCaps}, {"WINMM.DLL", "timeKillEvent", 1, w_timeKillEvent},
    {"WINMM.DLL", "timeSetEvent", 5, w_timeSetEvent}, {"WINMM.DLL", "timeGetTime", 0, w_timeGetTime},
    {"WINMM.DLL", "PlaySoundA", 3, u_one},
    {0},
};
