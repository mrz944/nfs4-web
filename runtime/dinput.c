/* DirectInput (A, v3/v5-style) for the system mouse, keyboard and joysticks.
 *
 * The host (SDL natively, the browser on web) feeds dinput_mouse_move /
 * dinput_mouse_button / dinput_key. Devices support immediate state
 * (GetDeviceState) and buffered events (GetDeviceData).
 *
 * Joysticks (gamepads, wheels): the page polls the Gamepad API every frame and
 * writes each controller's state into pads[] (rt_web_pads): 8 axes in -1..1 in
 * DIJOYSTATE order (X Y Z Rx Ry Rz slider slider), 32 buttons, the hat. The
 * game looks for joysticks once, at start, and like on Windows only the ones
 * connected then are offered (the game switches player 1 to a joystick it finds,
 * so a phantom one would take the keyboard away). The browser only shows a
 * controller after a button press: the page asks for one before Start.
 * RT_FAKEPAD=1 offers controller 1 natively (tests drive it with RT_INPUT JOY@/JOYB@).
 */
#define _GNU_SOURCE
#include "rt.h"
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

#define DI_OK 0
#define DIERR_GENERIC 0x80004005u
#define DIERR_NOTINITIALIZED 0x80070015u
#define DI_BUFFEROVERFLOW 1u

typedef struct { const char *name; int nargs; hostfn fn; } Method;

enum { DEV_MOUSE = 1, DEV_KEYBOARD = 2, DEV_JOY = 3 };
typedef struct { uint32_t seq, connected, wheel; float axes[8]; uint32_t buttons; int32_t pov; char name[64]; } PadState;
#define NPADS 2
static PadState pads[NPADS];
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE void *rt_web_pads(void) { return pads; }
#endif
void *dinput_pads(void) { return pads; }       /* native hosts and the test input (display.c) */
#define JOY_GUID1 0x4e465331u                   /* guidInstance.Data1 of controller k: 'NFS1' + k */
typedef struct { uint32_t ofs, data, time, seq; } Ev;
#define QN 256
typedef struct {
    int kind, acquired;
    uint32_t bufsize;        /* DIPROP_BUFFERSIZE; 0 = unbuffered */
    Ev q[QN]; int qh, qt;
    int32_t dx, dy, dz;      /* mouse motion accumulated since last GetDeviceState */
    uint32_t notify;         /* SetEventNotification event handle */
    uint8_t btn[8];
    int pad;                 /* DEV_JOY: which controller */
    int32_t lo[8], hi[8];    /* DIPROP_RANGE per axis */
    uint32_t dead;           /* DIPROP_DEADZONE, 0..10000 */
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

/* ----------------------------------------------------------- joysticks */
static uint32_t joy_devtype(int k) {      /* DIDEVTYPE_JOYSTICK, subtype wheel (3) or gamepad (5), HID */
    return 4u | (pads[k].wheel ? 3u : 5u) << 8 | 0x10000u;
}
/* DIDEVICEINSTANCEA: dwSize, guidInstance, guidProduct, dwDevType, tszInstanceName[260],
 * tszProductName[260], guidFFDriver, wUsagePage, wUsage */
static void fill_instance(uint32_t p, int k) {
    memset(G + p, 0, 580);
    WR32(p, 580);
    WR32(p + 4, JOY_GUID1 + (uint32_t)k); WR32(p + 20, JOY_GUID1 + (uint32_t)k);
    WR32(p + 36, joy_devtype(k));
    const char *name = pads[k].name[0] ? pads[k].name : "Controller";
    snprintf(HSTR(p + 40), 260, "%.60s", name); snprintf(HSTR(p + 300), 260, "%.60s", name);
    WR16(p + 576, 1); WR16(p + 578, pads[k].wheel ? 4 : 5);       /* HID usage page 1: joystick 4 / gamepad 5 */
}

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
    if (d->kind == DEV_JOY) {
        WR32(p + 4, 1);                                  /* DIDC_ATTACHED (no force feedback) */
        WR32(p + 8, joy_devtype(d->pad)); WR32(p + 12, 8); WR32(p + 16, 32); WR32(p + 20, 1);   /* axes, buttons, POVs */
        return DI_OK;
    }
    WR32(p + 4, 1 | 4);                                  /* DIDC_ATTACHED | DIDC_EMULATED */
    WR32(p + 8, d->kind == DEV_MOUSE ? 0x12 : 0x13);     /* DIDEVTYPE_MOUSE / KEYBOARD */
    WR32(p + 12, d->kind == DEV_MOUSE ? 3 : 0);          /* axes */
    WR32(p + 16, d->kind == DEV_MOUSE ? 3 : 256);        /* buttons */
    return DI_OK;
}
/* DIPROPHEADER { dwSize, dwHeaderSize, dwObj, dwHow }: dwHow 0 = whole device, 1 = by offset */
static int axis_of(uint32_t hdr) { uint32_t how = RD32(hdr + 12), obj = RD32(hdr + 8); return how == 0 ? -1 : how == 1 && obj < 32 && obj % 4 == 0 ? (int)(obj / 4) : -2; }
static uint32_t d_SetProperty(CPU *c, const uint32_t *a) {
    /* rguidProp is a small integer constant (MAKEDIPROP): BUFFERSIZE 1, RANGE 4, DEADZONE 5, SATURATION 6 */
    Dev *d = dev_of(A(0)); uint32_t h = A(2);
    if (A(1) == 1) { d->bufsize = RD32(h + 16); if (d->bufsize > QN - 1) d->bufsize = QN - 1; }
    else if (A(1) == 4 && d->kind == DEV_JOY) {
        int k = axis_of(h);
        for (int i = 0; i < 8; i++) if (k == -1 || k == i) { d->lo[i] = (int32_t)RD32(h + 16); d->hi[i] = (int32_t)RD32(h + 20); }
    } else if (A(1) == 5 && d->kind == DEV_JOY && axis_of(h) == -1) d->dead = RD32(h + 16);
    return DI_OK;
}
static uint32_t d_GetProperty(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t h = A(2);
    if (A(1) == 1) { WR32(h + 16, d->bufsize); return DI_OK; }
    if (A(1) == 4 && d->kind == DEV_JOY) { int k = axis_of(h); if (k < 0) k = 0; WR32(h + 16, (uint32_t)d->lo[k]); WR32(h + 20, (uint32_t)d->hi[k]); return DI_OK; }
    if (A(1) == 5 && d->kind == DEV_JOY) { WR32(h + 16, d->dead); return DI_OK; }
    return DIERR_GENERIC;
}
static uint32_t d_GetDeviceState(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t n = A(1), p = A(2);
    if (!d->acquired) return 0x8007000Cu;                /* DIERR_NOTACQUIRED */
    memset(G + p, 0, n);
    pthread_mutex_lock(&di_mu);
    if (d->kind == DEV_JOY) {                            /* DIJOYSTATE(2): 8 axes, 4 POVs, 32 (128) buttons */
        PadState *s = &pads[d->pad];
        for (int i = 0; i < 8 && 4 * (uint32_t)i + 4 <= n; i++) {
            float v = s->connected ? s->axes[i] : 0.0f;
            float dz = d->dead / 10000.0f;           /* deadzone around the centre, the rest rescaled */
            if (dz > 0 && dz < 1) v = v > dz ? (v - dz) / (1 - dz) : v < -dz ? (v + dz) / (1 - dz) : 0.0f;
            if (v < -1) v = -1; if (v > 1) v = 1;
            double lo = d->lo[i], hi = d->hi[i];
            WR32(p + 4 * (uint32_t)i, (uint32_t)(int32_t)lround(lo + (v + 1) * 0.5 * (hi - lo)));
        }
        for (uint32_t i = 0; i < 4 && 32 + 4 * i + 4 <= n; i++) WR32(p + 32 + 4 * i, i == 0 && s->connected && s->pov >= 0 ? (uint32_t)s->pov : 0xFFFFFFFFu);
        for (uint32_t i = 0; i < 32 && 48 + i < n; i++) WR8(p + 48 + i, s->connected && (s->buttons >> i & 1) ? 0x80 : 0);
    } else if (d->kind == DEV_MOUSE) {                   /* DIMOUSESTATE(2): lX lY lZ buttons[4|8] */
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
static uint32_t d_SetDataFormat(CPU *c, const uint32_t *a) {
    /* DIDATAFORMAT { dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf }: the state comes
     * back in the standard layouts (DIMOUSESTATE, 256 keys, DIJOYSTATE(2)), which is what the game asks */
    Dev *d = dev_of(A(0));
    if (d->kind == DEV_JOY) rt_log("DIJoystick::SetDataFormat(%u bytes, %u objects, flags %#x)", RD32(A(1) + 12), RD32(A(1) + 16), RD32(A(1) + 8));
    return DI_OK;
}
static uint32_t d_SetEventNotification(CPU *c, const uint32_t *a) { dev_of(A(0))->notify = A(1); return DI_OK; }
static uint32_t d_GetDeviceInfo(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t p = A(1), n = RD32(p);
    memset(G + p + 4, 0, n > 4 ? n - 4 : 0);
    if (d->kind == DEV_JOY) { fill_instance(p, d->pad); return DI_OK; }
    WR32(p + 36, d->kind == DEV_MOUSE ? 0x12 : 0x13);
    snprintf(HSTR(p + 40), 260, d->kind == DEV_MOUSE ? "Mouse" : "Keyboard");
    snprintf(HSTR(p + 300), 260, d->kind == DEV_MOUSE ? "Mouse" : "Keyboard");
    return DI_OK;
}
/* EnumObjects: the axes, the hat and the buttons, so the game's controller setup can list them */
static const char *axis_names[8] = { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation", "Slider", "Slider 2" };
static const uint32_t axis_guid[8] = { 0xA36D02E0u, 0xA36D02E1u, 0xA36D02E2u, 0xA36D02F4u, 0xA36D02F5u, 0xA36D02E3u, 0xA36D02E4u, 0xA36D02E4u };
static void write_guid(uint32_t p, uint32_t d1) {   /* {d1-C9F3-11CF-BFC7-444553540000}: DirectInput's object GUIDs */
    static const uint8_t tail[12] = { 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    WR32(p, d1); memcpy(G + p + 4, tail, 12);
}
/* object k: 0-7 the axes, 8 the hat, 9-40 the buttons. DIDEVICEOBJECTINSTANCEA: dwSize,
 * guidType, dwOfs, dwType, dwFlags, tszName[260] (+ DirectInput 5 fields, left zero) */
#define NOBJ (8 + 1 + 32)
static uint32_t obj_type(int k) { return k < 8 ? 0x2u | (uint32_t)k << 8 : k == 8 ? 0x10u : 0x4u | (uint32_t)(k - 9) << 8; }
static uint32_t obj_ofs(int k) { return k < 8 ? 4u * (uint32_t)k : k == 8 ? 32u : 48u + (uint32_t)(k - 9); }
static void fill_object(uint32_t o, uint32_t size, int k) {
    if (size < 292 || size > 316) size = 316;
    memset(G + o, 0, size);
    WR32(o, size); write_guid(o + 4, k < 8 ? axis_guid[k] : k == 8 ? 0xA36D02F2u : 0xA36D02F0u);
    WR32(o + 20, obj_ofs(k)); WR32(o + 24, obj_type(k));
    if (k < 8) snprintf(HSTR(o + 32), 260, "%s", axis_names[k]);
    else if (k == 8) snprintf(HSTR(o + 32), 260, "Hat");
    else snprintf(HSTR(o + 32), 260, "Button %d", k - 8);
}
static uint32_t d_EnumObjects(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t cb = A(1), ref = A(2), flags = A(3);
    if (d->kind != DEV_JOY || !cb) return DI_OK;
    uint32_t o = galloc(316);
    for (int k = 0; k < NOBJ; k++) {
        if (flags && !(flags & obj_type(k) & 0xFF)) continue;   /* DIDFT_AXIS 3, BUTTON 0xC, POV 0x10 */
        fill_object(o, 316, k);
        if (!guest_call(c, cb, 2, o, ref)) break;      /* DIENUM_STOP */
    }
    gfree(o);
    return DI_OK;
}
static uint32_t d_GetObjectInfo(CPU *c, const uint32_t *a) {
    Dev *d = dev_of(A(0)); uint32_t o = A(1), obj = A(2), how = A(3);
    if (d->kind != DEV_JOY) return DIERR_GENERIC;
    for (int k = 0; k < NOBJ; k++)                       /* dwHow 1 = by offset, 2 = by type id */
        if ((how == 1 && obj_ofs(k) == obj) || (how == 2 && (obj_type(k) & 0xFFFF00FFu) == (obj & 0xFFFF00FFu))) { fill_object(o, RD32(o), k); return DI_OK; }
    return 0x80070002u;                                  /* DIERR_OBJECTNOTFOUND */
}

static const Method dev_methods[] = {
    {"QueryInterface", 3, m_unimpl}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"GetCapabilities", 2, d_GetCapabilities}, {"EnumObjects", 4, d_EnumObjects}, {"GetProperty", 3, d_GetProperty},
    {"SetProperty", 3, d_SetProperty}, {"Acquire", 1, d_Acquire}, {"Unacquire", 1, d_Unacquire},
    {"GetDeviceState", 3, d_GetDeviceState}, {"GetDeviceData", 5, d_GetDeviceData}, {"SetDataFormat", 2, d_SetDataFormat},
    {"SetEventNotification", 2, d_SetEventNotification}, {"SetCooperativeLevel", 3, m_ok}, {"GetObjectInfo", 4, d_GetObjectInfo},
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
    int kind = d1 == 0x6F1D2B60u ? DEV_MOUSE : d1 == 0x6F1D2B61u ? DEV_KEYBOARD : d1 - JOY_GUID1 < NPADS ? DEV_JOY : 0;
    if (!kind || ndevs == 8) { rt_log("DirectInput::CreateDevice(%08x) -> not available", d1); WR32(A(2), 0); return 0x80040154u; }
    pthread_mutex_lock(&di_mu);
    Dev *d = &devs[ndevs]; memset(d, 0, sizeof *d); d->kind = kind;
    if (kind == DEV_JOY) { d->pad = (int)(d1 - JOY_GUID1); for (int i = 0; i < 8; i++) { d->lo[i] = 0; d->hi[i] = 65535; } }
    pthread_mutex_unlock(&di_mu);
    static Method m[sizeof dev_methods / sizeof *dev_methods]; static int init;
    if (!init) { memcpy(m, dev_methods, sizeof m); m[0].fn = dev_qi; init = 1; }
    uint32_t o = com_new(kind == DEV_MOUSE ? "DIMouse" : kind == DEV_JOY ? "DIJoystick" : "DIKeyboard", m, (int)(sizeof m / sizeof *m), (uint32_t)ndevs);
    ndevs++;
    rt_log("DirectInput::CreateDevice(%s) -> %#x", kind == DEV_MOUSE ? "SysMouse" : kind == DEV_JOY ? "controller" : "SysKeyboard", o);
    WR32(A(2), o);
    return DI_OK;
}
static uint32_t di_EnumDevices(CPU *c, const uint32_t *a) {
    /* the game asks for joysticks (type 4); mouse and keyboard it creates itself */
    uint32_t type = A(1), cb = A(2), ref = A(3), n = 0;
    if ((type == 0 || type == 4) && cb) {
#ifdef __EMSCRIPTEN__
        for (int i = 0; i < 60 && !__atomic_load_n(&pads[0].seq, __ATOMIC_ACQUIRE); i++) usleep(5000);   /* the page's first write */
#endif
        uint32_t p = galloc(580);                        /* DIDEVICEINSTANCEA (DirectInput 5) */
        if (getenv("RT_FAKEPAD") && !pads[0].connected) { pads[0].connected = 1; pads[0].pov = -1; snprintf(pads[0].name, sizeof pads[0].name, "Test controller"); }
        for (int k = 0; k < NPADS; k++) {
            if (!pads[k].connected) break;
            fill_instance(p, k); n++;
            if (!guest_call(c, cb, 2, p, ref)) break;    /* DIENUM_STOP */
        }
        gfree(p);
    }
    rt_log("DirectInput::EnumDevices(type=%u) -> %u controller(s)%s%s", type, n, pads[0].connected ? ": " : "", pads[0].connected ? pads[0].name : "");
    return DI_OK;
}
static uint32_t di_GetDeviceStatus(CPU *c, const uint32_t *a) {
    uint32_t d1 = RD32(A(1));
    return (d1 == 0x6F1D2B60u || d1 == 0x6F1D2B61u || d1 - JOY_GUID1 < NPADS) ? DI_OK : 1u; /* DI_NOTATTACHED */
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
