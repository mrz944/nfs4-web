/* Minimal DirectDraw (COM) for the software thrash driver.
 *
 * Objects live in guest memory: [vtbl ptr][host index]. Every vtable slot
 * is a thunk; unimplemented methods are logged with their arguments.
 * The primary surface is presented by present_frame() (PPM dumps natively,
 * a WebGL texture upload in the browser build).
 */
#define _GNU_SOURCE
#include "rt.h"
#include <sys/stat.h>

#define DD_OK 0
#define E_NOINTERFACE 0x80004002u
#define DDERR_GENERIC 0x80004005u

typedef struct { const char *name; int nargs; hostfn fn; } Method;

/* -------------------------------------------------------------- surfaces */
typedef struct {
    uint32_t obj;          /* guest COM object */
    int w, h, bpp, pitch;
    uint32_t mem;          /* guest pixel memory */
    uint32_t back;         /* attached back buffer object (primary flip chain) */
    int primary;
    uint32_t caps;
    uint32_t refs;
} Surface;

#define MAX_SURF 512
static Surface surfs[MAX_SURF];
static int nsurf;
static Surface *surf_of(uint32_t obj) {
    uint32_t i = RD32(obj + 4);
    return i < (uint32_t)nsurf ? &surfs[i] : NULL;
}

/* display mode */
static int mode_w = 640, mode_h = 480, mode_bpp = 16;

/* ------------------------------------------------------------- frames */
void present_frame(uint32_t mem, int w, int h, int pitch, int bpp);   /* display.c */

/* ------------------------------------------------------------ COM glue */
static uint32_t com_new(const char *iface, const Method *m, int n, uint32_t host_index) {
    uint32_t vt = galloc(4 * (uint32_t)n), obj = galloc(16);
    for (int i = 0; i < n; i++) {
        char nm[96]; snprintf(nm, sizeof nm, "%s::%s", iface, m[i].name);
        WR32(vt + 4 * i, thunk_new(nm, m[i].fn, m[i].nargs));
    }
    WR32(obj, vt); WR32(obj + 4, host_index); WR32(obj + 8, 1);
    return obj;
}

static uint32_t m_unimpl(CPU *c, const uint32_t *a);   /* logs via thunk name */
static uint32_t m_ok(CPU *c, const uint32_t *a) { return DD_OK; }
static uint32_t m_addref(CPU *c, const uint32_t *a) { WR32(A(0) + 8, RD32(A(0) + 8) + 1); return RD32(A(0) + 8); }
static uint32_t m_release(CPU *c, const uint32_t *a) { uint32_t n = RD32(A(0) + 8); if (n) n--; WR32(A(0) + 8, n); return n; }

static void guid_str(uint32_t p, char *out) {
    sprintf(out, "%08x-%04x-%04x", RD32(p), RD16(p + 4), RD16(p + 6));
}

/* --------------------------------------------------------- surface API */
static uint32_t surface_new(int w, int h, int bpp, uint32_t caps, int primary);
static void fill_desc(Surface *s, uint32_t d) {
    uint32_t size = RD32(d);
    memset(G + d + 4, 0, (size ? size : 108) - 4);
    WR32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000);  /* CAPS HEIGHT WIDTH PITCH PIXELFORMAT */
    WR32(d + 8, (uint32_t)s->h); WR32(d + 12, (uint32_t)s->w); WR32(d + 16, (uint32_t)s->pitch);
    WR32(d + 36, s->mem);
    uint32_t pf = d + 72;
    WR32(pf, 32); WR32(pf + 4, 0x40); /* DDPF_RGB */
    WR32(pf + 12, (uint32_t)s->bpp);
    if (s->bpp == 16) { WR32(pf + 16, 0xF800); WR32(pf + 20, 0x07E0); WR32(pf + 24, 0x001F); }
    else { WR32(pf + 16, 0xFF0000); WR32(pf + 20, 0xFF00); WR32(pf + 24, 0xFF); }
    WR32(d + 104, s->caps);
}
static uint32_t s_Lock(CPU *c, const uint32_t *a) {
    Surface *s = surf_of(A(0));
    if (A(1)) rt_log("Surface::Lock with rect not supported");
    fill_desc(s, A(2));
    return DD_OK;
}
static uint32_t s_Unlock(CPU *c, const uint32_t *a) {
    Surface *s = surf_of(A(0));
    if (s->primary && !s->back) present_frame(s->mem, s->w, s->h, s->pitch, s->bpp);
    return DD_OK;
}
static uint32_t s_GetSurfaceDesc(CPU *c, const uint32_t *a) { fill_desc(surf_of(A(0)), A(1)); return DD_OK; }
static uint32_t s_GetPixelFormat(CPU *c, const uint32_t *a) {
    Surface *s = surf_of(A(0)); uint32_t tmp = galloc(124); WR32(tmp, 108);
    fill_desc(s, tmp); memcpy(G + A(1), G + tmp + 72, 32); gfree(tmp);
    return DD_OK;
}
static uint32_t s_GetCaps(CPU *c, const uint32_t *a) { WR32(A(1), surf_of(A(0))->caps); return DD_OK; }
static uint32_t s_GetAttachedSurface(CPU *c, const uint32_t *a) {
    Surface *s = surf_of(A(0));
    if (!s->back) { rt_log("GetAttachedSurface: none"); return DDERR_GENERIC; }
    WR32(A(2), s->back);
    return DD_OK;
}
static uint32_t s_Flip(CPU *c, const uint32_t *a) {
    Surface *s = surf_of(A(0)), *b = surf_of(s->back);
    uint32_t t = s->mem; s->mem = b->mem; b->mem = t;    /* swap buffers: front now shows old back */
    present_frame(s->mem, s->w, s->h, s->pitch, s->bpp);
    return DD_OK;
}
static uint32_t s_Blt(CPU *c, const uint32_t *a) {
    Surface *d = surf_of(A(0)), *src = A(2) ? surf_of(A(2)) : NULL;
    uint32_t flags = A(4);
    int dx0 = 0, dy0 = 0, dx1 = d->w, dy1 = d->h;
    if (A(1)) { dx0 = (int)RD32(A(1)); dy0 = (int)RD32(A(1) + 4); dx1 = (int)RD32(A(1) + 8); dy1 = (int)RD32(A(1) + 12); }
    int bpp = d->bpp / 8;
    if (flags & 0x400) {                                   /* DDBLT_COLORFILL */
        uint32_t col = RD32(A(5) + 80);
        for (int y = dy0; y < dy1; y++) for (int x = dx0; x < dx1; x++) {
            uint32_t p = d->mem + (uint32_t)(y * d->pitch + x * bpp);
            if (bpp == 2) WR16(p, (uint16_t)col); else WR32(p, col);
        }
    } else if (src) {
        int sx0 = 0, sy0 = 0, sx1 = src->w, sy1 = src->h;
        if (A(3)) { sx0 = (int)RD32(A(3)); sy0 = (int)RD32(A(3) + 4); sx1 = (int)RD32(A(3) + 8); sy1 = (int)RD32(A(3) + 12); }
        for (int y = dy0; y < dy1; y++) {
            int sy = sy0 + (y - dy0) * (sy1 - sy0) / (dy1 - dy0);
            for (int x = dx0; x < dx1; x++) {
                int sx = sx0 + (x - dx0) * (sx1 - sx0) / (dx1 - dx0);
                memcpy(G + d->mem + y * d->pitch + x * bpp, G + src->mem + sy * src->pitch + sx * bpp, bpp);
            }
        }
    }
    if (d->primary && !d->back) present_frame(d->mem, d->w, d->h, d->pitch, d->bpp);
    return DD_OK;
}
static uint32_t s_BltFast(CPU *c, const uint32_t *a) {
    Surface *d = surf_of(A(0)), *src = surf_of(A(3));
    int x0 = (int)A(1), y0 = (int)A(2), sx0 = 0, sy0 = 0, sx1 = src->w, sy1 = src->h;
    if (A(4)) { sx0 = (int)RD32(A(4)); sy0 = (int)RD32(A(4) + 4); sx1 = (int)RD32(A(4) + 8); sy1 = (int)RD32(A(4) + 12); }
    int bpp = d->bpp / 8;
    for (int y = sy0; y < sy1; y++)
        memcpy(G + d->mem + (y0 + y - sy0) * d->pitch + x0 * bpp, G + src->mem + y * src->pitch + sx0 * bpp, (size_t)(sx1 - sx0) * bpp);
    return DD_OK;
}
static uint32_t s_QueryInterface(CPU *c, const uint32_t *a) {
    char g[64]; guid_str(A(1), g);
    rt_log("Surface::QueryInterface(%s) -> same object (vtable covers Surface2/3)", g);
    WR32(A(0) + 8, RD32(A(0) + 8) + 1);
    WR32(A(2), A(0));
    return DD_OK;
}
static uint32_t s_IsLost(CPU *c, const uint32_t *a) { return DD_OK; }

static const Method surface_methods[] = {
    {"QueryInterface", 3, s_QueryInterface}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"AddAttachedSurface", 2, m_unimpl}, {"AddOverlayDirtyRect", 2, m_unimpl}, {"Blt", 6, s_Blt},
    {"BltBatch", 4, m_unimpl}, {"BltFast", 6, s_BltFast}, {"DeleteAttachedSurface", 3, m_unimpl},
    {"EnumAttachedSurfaces", 3, m_unimpl}, {"EnumOverlayZOrders", 4, m_unimpl}, {"Flip", 3, s_Flip},
    {"GetAttachedSurface", 3, s_GetAttachedSurface}, {"GetBltStatus", 2, m_ok}, {"GetCaps", 2, s_GetCaps},
    {"GetClipper", 2, m_unimpl}, {"GetColorKey", 3, m_unimpl}, {"GetDC", 2, m_unimpl},
    {"GetFlipStatus", 2, m_ok}, {"GetOverlayPosition", 3, m_unimpl}, {"GetPalette", 2, m_unimpl},
    {"GetPixelFormat", 2, s_GetPixelFormat}, {"GetSurfaceDesc", 2, s_GetSurfaceDesc}, {"Initialize", 3, m_unimpl},
    {"IsLost", 1, s_IsLost}, {"Lock", 5, s_Lock}, {"ReleaseDC", 2, m_unimpl}, {"Restore", 1, m_ok},
    {"SetClipper", 2, m_ok}, {"SetColorKey", 3, m_ok}, {"SetOverlayPosition", 3, m_unimpl},
    {"SetPalette", 2, m_ok}, {"Unlock", 2, s_Unlock}, {"UpdateOverlay", 6, m_unimpl},
    {"UpdateOverlayDisplay", 2, m_unimpl}, {"UpdateOverlayZOrder", 3, m_unimpl},
    /* IDirectDrawSurface2 / 3 */
    {"GetDDInterface", 2, m_unimpl}, {"PageLock", 2, m_ok}, {"PageUnlock", 2, m_ok},
    {"SetSurfaceDesc", 3, m_unimpl},
};

static uint32_t surface_new(int w, int h, int bpp, uint32_t caps, int primary) {
    if (nsurf == MAX_SURF) rt_fatal("too many surfaces");
    Surface *s = &surfs[nsurf];
    s->w = w; s->h = h; s->bpp = bpp; s->pitch = (w * bpp / 8 + 3) & ~3; s->caps = caps; s->primary = primary;
    s->mem = galloc((uint32_t)(s->pitch * h));
    s->obj = com_new("Surface", surface_methods, (int)(sizeof surface_methods / sizeof *surface_methods), (uint32_t)nsurf);
    nsurf++;
    return s->obj;
}

/* ----------------------------------------------------------- clipper */
static const Method clipper_methods[] = {
    {"QueryInterface", 3, m_unimpl}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"GetClipList", 4, m_unimpl}, {"GetHWnd", 2, m_unimpl}, {"Initialize", 3, m_ok},
    {"IsClipListChanged", 2, m_ok}, {"SetClipList", 3, m_ok}, {"SetHWnd", 3, m_ok},
};

/* ----------------------------------------------------------- IDirectDraw */
static uint32_t dd_CreateSurface(CPU *c, const uint32_t *a) {
    uint32_t d = A(1), flags = RD32(d + 4), caps = RD32(d + 104);
    int w = mode_w, h = mode_h, bpp = mode_bpp;
    if (flags & 0x4) w = (int)RD32(d + 12);
    if (flags & 0x2) h = (int)RD32(d + 8);
    if (flags & 0x1000) bpp = (int)RD32(d + 72 + 12);
    int primary = (caps & 0x200) != 0;                    /* DDSCAPS_PRIMARYSURFACE */
    if (primary) { w = mode_w; h = mode_h; bpp = mode_bpp; }
    uint32_t obj = surface_new(w, h, bpp, caps, primary);
    if (primary && (flags & 0x20) && RD32(d + 20)) {      /* DDSD_BACKBUFFERCOUNT */
        uint32_t back = surface_new(w, h, bpp, 0x4 /* BACKBUFFER */, 0);
        surf_of(obj)->back = back;
    }
    rt_log("CreateSurface %dx%d %dbpp caps=%#x%s -> %#x", w, h, bpp, caps, surf_of(obj)->back ? " +backbuffer" : "", obj);
    WR32(A(2), obj);
    return DD_OK;
}
static uint32_t dd_SetCooperativeLevel(CPU *c, const uint32_t *a) { rt_log("SetCooperativeLevel(%#x)", A(2)); return DD_OK; }
static uint32_t dd_SetDisplayMode(CPU *c, const uint32_t *a) {
    mode_w = (int)A(1); mode_h = (int)A(2); mode_bpp = (int)A(3);
    rt_log("SetDisplayMode(%d, %d, %d)", mode_w, mode_h, mode_bpp);
    return DD_OK;
}
static uint32_t dd2_SetDisplayMode(CPU *c, const uint32_t *a) { return dd_SetDisplayMode(c, a); }
static uint32_t dd_GetDisplayMode(CPU *c, const uint32_t *a) {
    Surface tmp = {.w = mode_w, .h = mode_h, .bpp = mode_bpp, .pitch = mode_w * mode_bpp / 8};
    fill_desc(&tmp, A(1)); return DD_OK;
}
static uint32_t dd_EnumDisplayModes(CPU *c, const uint32_t *a) {
    static const int modes[][3] = {{640, 480, 16}, {800, 600, 16}, {1024, 768, 16}, {640, 480, 32}, {800, 600, 32}, {1024, 768, 32}};
    uint32_t d = galloc(124);
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        Surface tmp = {.w = modes[i][0], .h = modes[i][1], .bpp = modes[i][2], .pitch = modes[i][0] * modes[i][2] / 8};
        WR32(d, 108); fill_desc(&tmp, d);
        if (!guest_call(c, A(4), 2, d, A(3))) break;          /* DDENUMRET_CANCEL */
    }
    gfree(d);
    return DD_OK;
}
static uint32_t dd_GetCaps(CPU *c, const uint32_t *a) {
    for (int k = 1; k <= 2; k++) if (A(k)) { uint32_t n = RD32(A(k)); memset(G + A(k) + 4, 0, n > 4 ? n - 4 : 0); }
    return DD_OK;
}
static uint32_t dd_CreateClipper(CPU *c, const uint32_t *a) {
    WR32(A(2), com_new("Clipper", clipper_methods, (int)(sizeof clipper_methods / sizeof *clipper_methods), 0));
    return DD_OK;
}
static uint32_t dd2_obj;
static uint32_t dd_QueryInterface(CPU *c, const uint32_t *a);
static uint32_t dd_GetAvailableVidMem(CPU *c, const uint32_t *a) {
    if (A(2)) WR32(A(2), 32u << 20); if (A(3)) WR32(A(3), 30u << 20); return DD_OK;
}
static const Method dd_methods[] = {
    {"QueryInterface", 3, dd_QueryInterface}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"Compact", 1, m_ok}, {"CreateClipper", 4, dd_CreateClipper}, {"CreatePalette", 5, m_unimpl},
    {"CreateSurface", 4, dd_CreateSurface}, {"DuplicateSurface", 3, m_unimpl},
    {"EnumDisplayModes", 5, dd_EnumDisplayModes}, {"EnumSurfaces", 5, m_unimpl}, {"FlipToGDISurface", 1, m_ok},
    {"GetCaps", 3, dd_GetCaps}, {"GetDisplayMode", 2, dd_GetDisplayMode}, {"GetFourCCCodes", 3, m_unimpl},
    {"GetGDISurface", 2, m_unimpl}, {"GetMonitorFrequency", 2, m_unimpl}, {"GetScanLine", 2, m_unimpl},
    {"GetVerticalBlankStatus", 2, m_unimpl}, {"Initialize", 2, m_ok}, {"RestoreDisplayMode", 1, m_ok},
    {"SetCooperativeLevel", 3, dd_SetCooperativeLevel}, {"SetDisplayMode", 4, dd_SetDisplayMode},
    {"WaitForVerticalBlank", 3, m_ok}, {"GetAvailableVidMem", 4, dd_GetAvailableVidMem},
};

/* IDirectDraw2: same layout, SetDisplayMode takes (w, h, bpp, refresh, flags) */
static Method dd2_methods[sizeof dd_methods / sizeof *dd_methods];
static uint32_t dd_QueryInterface(CPU *c, const uint32_t *a) {
    char g[64]; guid_str(A(1), g);
    if (RD32(A(1)) == 0xB3A6F3E0u) {                       /* IID_IDirectDraw2 */
        if (!dd2_obj) {
            memcpy(dd2_methods, dd_methods, sizeof dd_methods);
            dd2_methods[21] = (Method){"SetDisplayMode", 6, dd2_SetDisplayMode};
            dd2_obj = com_new("IDirectDraw2", dd2_methods, (int)(sizeof dd2_methods / sizeof *dd2_methods), 0);
        }
        WR32(A(2), dd2_obj);
        return DD_OK;
    }
    rt_log("IDirectDraw::QueryInterface(%s) not supported", g);
    WR32(A(2), 0);
    return E_NOINTERFACE;
}

static uint32_t m_unimpl(CPU *c, const uint32_t *a) {
    /* thunk name is not passed in; log the return address so the call site is findable */
    rt_log("%s: unimplemented (this=%#x a1=%#x a2=%#x a3=%#x, ret=%#x)",
           tls_thunk_name, a[0], a[1], a[2], a[3], RD32(c->r[ESP]));
    return DDERR_GENERIC;
}

static uint32_t x_DirectDrawCreate(CPU *c, const uint32_t *a) {
    uint32_t dd = com_new("IDirectDraw", dd_methods, (int)(sizeof dd_methods / sizeof *dd_methods), 0);
    rt_log("DirectDrawCreate -> %#x", dd);
    WR32(A(1), dd);
    return DD_OK;
}
static uint32_t x_DirectDrawEnumerateA(CPU *c, const uint32_t *a) {
    uint32_t desc = gstrdup("Primary Display Driver"), name = gstrdup("display");
    guest_call(c, A(0), 4, 0u, desc, name, A(1));   /* LPDDENUMCALLBACKA(guid, desc, name, ctx) */
    return DD_OK;
}

const ImportDef imp_ddraw[] = {
    {"DDRAW.DLL", "DirectDrawCreate", 3, x_DirectDrawCreate},
    {"DDRAW.DLL", "DirectDrawEnumerateA", 2, x_DirectDrawEnumerateA},
    {0},
};
