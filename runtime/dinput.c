/* DirectInput (A, v3/v5-style) for the system mouse and keyboard.
 *
 * The host (SDL natively, the browser on web) feeds dinput_mouse_move /
 * dinput_mouse_button / dinput_key. Devices support immediate state
 * (GetDeviceState) and buffered events (GetDeviceData).
 * No joysticks are enumerated.
 */
#define _GNU_SOURCE
#include "rt.h"
#include <pthread.h>
#include <time.h>

#define DI_OK 0
#define DIERR_GENERIC 0x80004005u
#define DIERR_NOTINITIALIZED 0x80070015u
#define DI_BUFFEROVERFLOW 1u

typedef struct { const char *name; int nargs; hostfn fn; } Method;

enum { DEV_MOUSE = 1, DEV_KEYBOARD = 2 };
typedef struct { uint32_t ofs, data, time, seq; } Ev;
#define QN 256
typedef struct {
    int kind, acquired;
    uint32_t bufsize;        /* DIPROP_BUFFERSIZE; 0 = unbuffered */
    Ev q[QN]; int qh, qt;
    int32_t dx, dy, dz;      /* mouse motion accumulated since last GetDeviceState */
    uint32_t notify;         /* SetEventNotification event handle */
    uint8_t btn[8];
} Dev;

static Dev devs[8]; static int ndevs;
static pthread_mutex_t di_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t dik[256];
static uint32_t seq;

static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void push(Dev *d, uint32_t ofs, uint32_t data) {
    if (!d->acquired || !d->bufsize) return;
    int nt = (d->qt + 1) % QN;
    if (nt == d->qh) return;
    d->q[d->qt] = (Ev){ofs, data, now_ms(), ++seq};
    d->qt = nt;
    if (d->notify) event_signal(d->notify);
}

/* ---------------------------------------------------------- host feed */
/* The game scales mickeys by ~0.45; scale up so its cursor tracks the host
 * pointer in 640x480 game coordinates. RT_MOUSE_SCALE overrides. */
static double mouse_scale = -1, frac_x, frac_y;
void dinput_mouse_move(int dx0, int dy0, int dz) {
    if (mouse_scale < 0) { const char *e = getenv("RT_MOUSE_SCALE"); mouse_scale = e ? atof(e) : 2.2; }
    double fx = dx0 * mouse_scale + frac_x, fy = dy0 * mouse_scale + frac_y;
    int dx = (int)fx, dy = (int)fy; frac_x = fx - dx; frac_y = fy - dy;
    pthread_mutex_lock(&di_mu);
    for (int i = 0; i < ndevs; i++) if (devs[i].kind == DEV_MOUSE) {
        Dev *d = &devs[i];
        d->dx += dx; d->dy += dy; d->dz += dz;
        if (dx) push(d, 0, (uint32_t)dx);       /* DIMOFS_X */
        if (dy) push(d, 4, (uint32_t)dy);       /* DIMOFS_Y */
        if (dz) push(d, 8, (uint32_t)dz);       /* DIMOFS_Z */
    }
    pthread_mutex_unlock(&di_mu);
}
void dinput_mouse_button(int b, int down) {   /* b: 0 left, 1 right, 2 middle */
    if (b < 0 || b > 7) return;
    pthread_mutex_lock(&di_mu);
    for (int i = 0; i < ndevs; i++) if (devs[i].kind == DEV_MOUSE) {
        devs[i].btn[b] = down ? 0x80 : 0;
        push(&devs[i], 12u + (uint32_t)b, down ? 0x80u : 0u);   /* DIMOFS_BUTTON0+b */
    }
    pthread_mutex_unlock(&di_mu);
}
void dinput_key(uint8_t code, int down) {      /* code: DIK_* (scan code, |0x80 for extended keys) */
    pthread_mutex_lock(&di_mu);
    dik[code] = down ? 0x80 : 0;
    for (int i = 0; i < ndevs; i++) if (devs[i].kind == DEV_KEYBOARD) push(&devs[i], code, down ? 0x80u : 0u);
    pthread_mutex_unlock(&di_mu);
}

/* ------------------------------------------------------------- COM glue */
static uint32_t com_new(const char *iface, const Method *m, int n, uint32_t idx) {
    uint32_t vt = galloc(4 * (uint32_t)n), obj = galloc(16);
    for (int i = 0; i < n; i++) {
        char nm[96]; snprintf(nm, sizeof nm, "%s::%s", iface, m[i].name);
        WR32(vt + 4 * i, thunk_new(nm, m[i].fn, m[i].nargs));
    }
    WR32(obj, vt); WR32(obj + 4, idx); WR32(obj + 8, 1);
    return obj;
}
static uint32_t m_ok(CPU *c, const uint32_t *a) { return DI_OK; }
static uint32_t m_unimpl(CPU *c, const uint32_t *a) {
    rt_log("%s: unimplemented (this=%#x a1=%#x a2=%#x)", tls_thunk_name, a[0], a[1], a[2]);
    return DIERR_GENERIC;
}
static uint32_t m_addref(CPU *c, const uint32_t *a) { WR32(A(0) + 8, RD32(A(0) + 8) + 1); return RD32(A(0) + 8); }
static uint32_t m_release(CPU *c, const uint32_t *a) { uint32_t n = RD32(A(0) + 8); if (n) n--; WR32(A(0) + 8, n); return n; }
static Dev *dev_of(uint32_t obj) { uint32_t i = RD32(obj + 4); return i < (uint32_t)ndevs ? &devs[i] : NULL; }

/* ------------------------------------------------------------- devices */
static uint32_t d_Acquire(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&di_mu); Dev *d = dev_of(A(0)); d->acquired = 1; d->dx = d->dy = d->dz = 0; pthread_mutex_unlock(&di_mu);
    return DI_OK;
}
static uint32_t d_Unacquire(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&di_mu); dev_of(A(0))->acquired = 0; pthread_mutex_unlock(&di_mu); return DI_OK;
}
static uint32_t d_GetCapabilities(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t p = A(1);
    uint32_t n = RD32(p); memset(G + p + 4, 0, n > 4 ? n - 4 : 40);
    WR32(p + 4, 1 | 4);                                  /* DIDC_ATTACHED | DIDC_EMULATED */
    WR32(p + 8, d->kind == DEV_MOUSE ? 0x12 : 0x13);     /* DIDEVTYPE_MOUSE / KEYBOARD */
    WR32(p + 12, d->kind == DEV_MOUSE ? 3 : 0);          /* axes */
    WR32(p + 16, d->kind == DEV_MOUSE ? 3 : 256);        /* buttons */
    return DI_OK;
}
static uint32_t d_SetProperty(CPU *c, const uint32_t *a) {
    /* rguidProp is a small integer constant (MAKEDIPROP); DIPROP_BUFFERSIZE = 1 */
    if (A(1) == 1) { Dev *d = dev_of(A(0)); d->bufsize = RD32(A(2) + 16); if (d->bufsize > QN - 1) d->bufsize = QN - 1; }
    return DI_OK;
}
static uint32_t d_GetProperty(CPU *c, const uint32_t *a) {
    if (A(1) == 1) { WR32(A(2) + 16, dev_of(A(0))->bufsize); return DI_OK; }
    return DIERR_GENERIC;
}
static uint32_t d_GetDeviceState(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t n = A(1), p = A(2);
    if (!d->acquired) return 0x8007000Cu;                /* DIERR_NOTACQUIRED */
    memset(G + p, 0, n);
    pthread_mutex_lock(&di_mu);
    if (d->kind == DEV_MOUSE) {                          /* DIMOUSESTATE(2): lX lY lZ buttons[4|8] */
        if (n >= 12) { WR32(p, (uint32_t)d->dx); WR32(p + 4, (uint32_t)d->dy); WR32(p + 8, (uint32_t)d->dz); }
        for (uint32_t i = 0; i < 8 && 12 + i < n; i++) WR8(p + 12 + i, d->btn[i]);
        d->dx = d->dy = d->dz = 0;
    } else {
        memcpy(G + p, dik, n < 256 ? n : 256);
    }
    pthread_mutex_unlock(&di_mu);
    return DI_OK;
}
static uint32_t d_GetDeviceData(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t objsz = A(1), out = A(2), inout = A(3), flags = A(4);
    if (!d->acquired) return 0x8007000Cu;
    uint32_t want = RD32(inout), got = 0;
    pthread_mutex_lock(&di_mu);
    int h = d->qh;
    while (h != d->qt && (got < want || !out)) {
        if (out) {
            Ev *e = &d->q[h];
            WR32(out + got * objsz, e->ofs); WR32(out + got * objsz + 4, e->data);
            WR32(out + got * objsz + 8, e->time); WR32(out + got * objsz + 12, e->seq);
        }
        got++; h = (h + 1) % QN;
        if (!out && got >= want) break;
    }
    if (!(flags & 1)) d->qh = h;                          /* DIGDD_PEEK leaves events queued */
    if (d->kind == DEV_MOUSE && !(flags & 1)) d->dx = d->dy = d->dz = 0;
    pthread_mutex_unlock(&di_mu);
    WR32(inout, got);
    return DI_OK;
}
static uint32_t d_SetEventNotification(CPU *c, const uint32_t *a) { dev_of(A(0))->notify = A(1); return DI_OK; }
static uint32_t d_GetDeviceInfo(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t p = A(1), n = RD32(p);
    memset(G + p + 4, 0, n > 4 ? n - 4 : 0);
    WR32(p + 36, d->kind == DEV_MOUSE ? 0x12 : 0x13);
    snprintf(HSTR(p + 40), 260, d->kind == DEV_MOUSE ? "Mouse" : "Keyboard");
    snprintf(HSTR(p + 300), 260, d->kind == DEV_MOUSE ? "Mouse" : "Keyboard");
    return DI_OK;
}

static const Method dev_methods[] = {
    {"QueryInterface", 3, m_unimpl}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"GetCapabilities", 2, d_GetCapabilities}, {"EnumObjects", 4, m_ok}, {"GetProperty", 3, d_GetProperty},
    {"SetProperty", 3, d_SetProperty}, {"Acquire", 1, d_Acquire}, {"Unacquire", 1, d_Unacquire},
    {"GetDeviceState", 3, d_GetDeviceState}, {"GetDeviceData", 5, d_GetDeviceData}, {"SetDataFormat", 2, m_ok},
    {"SetEventNotification", 2, d_SetEventNotification}, {"SetCooperativeLevel", 3, m_ok}, {"GetObjectInfo", 4, m_unimpl},
    {"GetDeviceInfo", 2, d_GetDeviceInfo}, {"RunControlPanel", 3, m_ok}, {"Initialize", 4, m_ok},
    /* IDirectInputDevice2A */
    {"CreateEffect", 5, m_unimpl}, {"EnumEffects", 4, m_ok}, {"GetEffectInfo", 3, m_unimpl},
    {"GetForceFeedbackState", 2, m_unimpl}, {"SendForceFeedbackCommand", 2, m_unimpl},
    {"EnumCreatedEffectObjects", 4, m_ok}, {"Escape", 2, m_unimpl}, {"Poll", 1, m_ok}, {"SendDeviceData", 5, m_unimpl},
};

static uint32_t dev_qi(CPU *c, const uint32_t *a) {
    /* IDirectInputDevice2A shares the vtable prefix: hand back the same object */
    WR32(A(0) + 8, RD32(A(0) + 8) + 1);
    WR32(A(2), A(0));
    return DI_OK;
}

/* ------------------------------------------------------------ IDirectInput */
static uint32_t di_CreateDevice(CPU *c, const uint32_t *a) {
    uint32_t g = A(1), d1 = RD32(g);
    int kind = d1 == 0x6F1D2B60u ? DEV_MOUSE : d1 == 0x6F1D2B61u ? DEV_KEYBOARD : 0;
    if (!kind || ndevs == 8) { rt_log("DirectInput::CreateDevice(%08x) -> not available", d1); WR32(A(2), 0); return 0x80040154u; }
    pthread_mutex_lock(&di_mu);
    Dev *d = &devs[ndevs]; memset(d, 0, sizeof *d); d->kind = kind;
    pthread_mutex_unlock(&di_mu);
    static Method m[sizeof dev_methods / sizeof *dev_methods]; static int init;
    if (!init) { memcpy(m, dev_methods, sizeof m); m[0].fn = dev_qi; init = 1; }
    uint32_t o = com_new(kind == DEV_MOUSE ? "DIMouse" : "DIKeyboard", m, (int)(sizeof m / sizeof *m), (uint32_t)ndevs);
    ndevs++;
    rt_log("DirectInput::CreateDevice(%s) -> %#x", kind == DEV_MOUSE ? "SysMouse" : "SysKeyboard", o);
    WR32(A(2), o);
    return DI_OK;
}
static uint32_t di_EnumDevices(CPU *c, const uint32_t *a) {
    /* only joysticks are interesting to the game here; report none */
    rt_log("DirectInput::EnumDevices(type=%u) -> none", A(1));
    return DI_OK;
}
static uint32_t di_GetDeviceStatus(CPU *c, const uint32_t *a) {
    uint32_t d1 = RD32(A(1));
    return (d1 == 0x6F1D2B60u || d1 == 0x6F1D2B61u) ? DI_OK : 1u; /* DI_NOTATTACHED */
}
static const Method di_methods[] = {
    {"QueryInterface", 3, m_unimpl}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"CreateDevice", 4, di_CreateDevice}, {"EnumDevices", 5, di_EnumDevices},
    {"GetDeviceStatus", 2, di_GetDeviceStatus}, {"RunControlPanel", 3, m_ok}, {"Initialize", 3, m_ok},
};

static uint32_t di_obj;
static uint32_t x_DirectInputCreateA(CPU *c, const uint32_t *a) {
    if (!di_obj) {
        di_obj = com_new("IDirectInput", di_methods, (int)(sizeof di_methods / sizeof *di_methods), 0);
        rt_log("DirectInputCreateA(version %#x) -> %#x", A(1), di_obj);
    } else WR32(di_obj + 8, RD32(di_obj + 8) + 1);
    WR32(A(2), di_obj);
    return DI_OK;
}
const ImportDef imp_dinput[] = { {"DINPUT.DLL", "DirectInputCreateA", 4, x_DirectInputCreateA}, {0} };
