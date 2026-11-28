/* Widescreen patch (docs/widescreen-plan.md): hooks on the game's view, projection and HUD code
 * (runtime/hooks.txt). 4:3 frames keep the original code path.
 *   RT_WS_LOG=1   log the hooks' arguments (each distinct set once, up to 64) */
#include "hooks.h"
#include <math.h>

static int ws_log = -1;
static int ws_logging(void) {
    if (ws_log < 0) { const char *e = getenv("RT_WS_LOG"); ws_log = e && *e && *e != '0' ? 64 : 0; }
    return ws_log > 0;
}

/* ---- Hor+ (P3) ----
 * 0x453F10: view setup (x, y, w, h, fov, n, flip), floats except flip, all on the stack, ret 0x1c.
 * Sets the half sizes 0x5b0958/0x5b095c = w/2, h/2, the centre 0x680f98/0x680f9c = x + w/2,
 * y + h/2, and the focal lengths 0x5b0950 = (w / tan(fov°)) / 2 (w/2 when fov is 0) and
 * 0x5b0954 = -0x5b0950 (swapped when flip): fov is the half-angle across the width, so a wider
 * screen kept the horizontal view and lost some vertical (Vert-).
 * Hor+: a view whose projection is the whole screen (the main view, each split-screen half, the
 * intro and other full-screen cameras: all made from the screen's record 0x853a10, w x h) gets
 * the focal length of a 4:3 screen of the same height, f * (h * 4/3) / w, when the screen is
 * wider than 4:3: the same vertical view as at 4:3, more at the sides. With Options > Graphics >
 * Wide Screen (0x7c3d5c; not in split screen, 0x7c4bec) the view is a 2:1 band of width w
 * (f_005197D0), so the reference is 2:1: up to 2:1 the band is the very picture it is at 4:3.
 * 4:3 screens, and views of their own size (the mirror, the front end's), keep the original. */
#define VIEW_MAIN   0x853a10u      /* {x, y, w, h, clip x, y, w, h}: w, h = the screen */
#define WIDE_SCREEN 0x7c3d5cu
#define SPLIT       0x7c4becu
#define FOCAL_X     0x5b0950u
#define FOCAL_Y     0x5b0954u
static float ws_last_scale = 1;    /* the last view's Hor+ factor (camera_view's zoom uses it) */

void hook_view_setup(CPU *c) {
    uint32_t a[8];
    for (int i = 0; i < 7; i++) a[i] = HOOK_ARG32(c, i);
    a[7] = RD32(c->r[ESP]);
    float w, h; memcpy(&w, &a[2], 4); memcpy(&h, &a[3], 4);
    if (ws_logging()) {
        static uint32_t seen[64][8]; static int nseen;
        int k = 0; while (k < nseen && memcmp(seen[k], a, sizeof a)) k++;
        if (k == nseen && nseen < 64) {
            memcpy(seen[nseen++], a, sizeof a); ws_log--;
            float x, y, fov, n; memcpy(&x, &a[0], 4); memcpy(&y, &a[1], 4); memcpy(&fov, &a[4], 4); memcpy(&n, &a[5], 4);
            rt_log("ws: view_setup(x %g, y %g, w %g, h %g, fov %g, n %g, flip %u) from 0x%x",
                   x, y, w, h, fov, n, a[6], a[7]);
        }
    }
    f_00453F10_orig(c);
    ws_last_scale = 1;
    if (w == (float)(int32_t)RD32(VIEW_MAIN + 8) && h == (float)(int32_t)RD32(VIEW_MAIN + 12) && h > 0) {
        float ref = RD32(WIDE_SCREEN) && RD32(SPLIT) != 1 ? 2.0f : 4.0f / 3.0f, s = ref * h / w;
        if (s < 1) {
            ws_last_scale = s;
            WRF32(FOCAL_X, RDF32(FOCAL_X) * s); WRF32(FOCAL_Y, RDF32(FOCAL_Y) * s);
        }
    }
}

/* 0x539F80(eax = view {type, player, ...}): the view's record (type 1: the mirror, 0x853a70;
 * else the main or split-screen one) through 0x51a2c0 -> 0x453F10, then, if the player's camera
 * has a zoom (camera + 0x54), focal x = w/2 * zoom and focal y = -h/2 * zoom * 1.348 (0x5a9798,
 * about 4/3: square at 4:3 only). Hor+ for those too: focal x times the view's factor. */
#define CAM_A 0x661958u
#define CAM_B 0x661950u
void hook_camera_view(CPU *c) {
    uint32_t view = c->r[EAX];
    f_00539F80_orig(c);
    if (ws_last_scale >= 1 || RD32(view) == 1) return;
    uint32_t p = RD32(view + 4) * 4, cam = RD32(CAM_A + p) ? RD32(CAM_A + p) : RD32(CAM_B + p);
    if (cam && (RD32(cam + 0x54) & 0x7fffffffu)) {
        WRF32(FOCAL_X, RDF32(FOCAL_X) * ws_last_scale);
        static uint32_t last; if (ws_logging() && RD32(cam + 0x54) != last) {
            last = RD32(cam + 0x54); ws_log--;
            rt_log("ws: camera_view zoom %g (camera type %u): focal %g, %g", RDF32(cam + 0x54), RD32(cam),
                   RDF32(FOCAL_X), RDF32(FOCAL_Y));
        }
    }
}

/* ---- screen modes (P2) ----
 * The driver's 16 mode records (glide_patch_driver) carry 4:3, 16:9 and 16:10 sizes. The game
 * listed and looked up only exact 4:3 modes (w * 3 == h * 4, at 0x51a4a5 and 0x51a66b), and saves
 * only the width (0x7c3d24, read back only at 0x4c2bb0 for the lookup). So for a mode that isn't
 * 4:3, the list entry's width (copied to 0x7c3d24 when picked, then config.dat) is
 * w | h << 16; the lookup decodes it. 4:3 picks keep a plain width, as do old configs, which
 * still get the 4:3 mode nearest their width. */
#define MODE_INFO_FN   0x9e801cu     /* -> driver info: +0x3c record count, +0x40 records (1-based) */
#define MODE_LIST_N    0x727524u     /* the Options list: count (at most 20), 276-byte entries */
#define MODE_ENTRY     276u
#define MODE_LIST      0x727528u     /*   { mode number, w, h, depth, buffers ok, label } */
#define MODE_LIST_CB   0x487720u     /* the game's list callback that adds an entry */
#define TRIPLEBUF      0x7c3d54u
#define DEPTH_FLAG     0x8287c0u

static uint32_t mode_info(CPU *c) {
    uint32_t r[8]; memcpy(r, c->r, sizeof r);
    PUSH32(0); call_indirect(c, RD32(MODE_INFO_FN));
    uint32_t info = c->r[EAX];
    memcpy(c->r, r, sizeof r);
    return info;
}
/* the game's tests for a record, but the 4:3 one: depth (15/16 bpp, or 32 when asked for and
 * allowed), available and not excluded, enough color buffers for Triple Buffering */
static int mode_ok(uint32_t rec, int want32) {
    uint32_t bpp = RD32(rec + 8), fl = RD32(rec + 16);
    int depth = bpp == 15 || bpp == 16 ? !want32 : bpp == 32 && want32;
    return depth && (fl & 1) && !(fl & 2) && (int)RD32(rec + 20) >= (int)RD32(TRIPLEBUF) + 2;
}

/* 0x51A390(eax = callback): for each usable record, callback(eax = number, edx = w, ebx = h,
 * ecx = bpp, push enough buffers for Triple Buffering), ret 4. Registers but eax kept, ret.
 * The original also skipped modes whose two buffers didn't fit half the game's video memory
 * budget (0x5befa4, read nowhere else): our renderer has no board memory, so that's dropped. */
void hook_mode_list(CPU *c) {
    uint32_t saved[8]; memcpy(saved, c->r, sizeof saved);
    uint32_t cb = c->r[EAX], info = mode_info(c), n = RD32(info + 0x3c), recs = RD32(info + 0x40);
    int want32 = !(RD32(DEPTH_FLAG) & 1);              /* 32-bit records count as usable */
    for (uint32_t i = 1; i <= n; i++) {
        uint32_t rec = recs + 40 * i, w = RD32(rec), h = RD32(rec + 4);
        if (!mode_ok(rec, RD32(rec + 8) == 32 && want32)) continue;
        uint32_t before = RD32(MODE_LIST_N), esp = c->r[ESP];
        c->r[EAX] = i; c->r[EDX] = w; c->r[EBX] = h; c->r[ECX] = RD32(rec + 8);
        PUSH32((int)RD32(TRIPLEBUF) + 2 <= (int)RD32(rec + 24));
        PUSH32(0x51a514u);                             /* the original's return address */
        call_indirect(c, cb);
        c->r[ESP] = esp;
        if (cb == MODE_LIST_CB && RD32(MODE_LIST_N) == before + 1 && w * 3 != h * 4)
            WR32(MODE_LIST + MODE_ENTRY * before + 4, w | h << 16);
    }
    memcpy(c->r, saved, sizeof saved);
    c->r[ESP] += 4;
}

/* 0x51A530(eax = saved width, edx = depth) -> eax = record number; ebx, ecx kept, ret. The
 * original: the usable 4:3 record nearest the width, else stops the game ("AMF=5 screen.c(563)").
 * Now: the exact size (w | h << 16), else the nearest width of the same aspect, else of 4:3
 * (plain widths: old configs, new players), else of any aspect. */
void hook_mode_lookup(CPU *c) {
    uint32_t want = c->r[EAX], ww = want & 0xffff, wh = want >> 16;
    int want32 = c->r[EDX] == 32 && !(RD32(DEPTH_FLAG) & 1);
    uint32_t info = mode_info(c), n = RD32(info + 0x3c), recs = RD32(info + 0x40);
    uint32_t best[3] = { 0, 0, 0 }, dist[3] = { 0, 0, 0 };   /* same aspect, 4:3, any */
    for (uint32_t i = 1; i <= n; i++) {
        uint32_t rec = recs + 40 * i, w = RD32(rec), h = RD32(rec + 4);
        if (!mode_ok(rec, want32)) continue;
        if (wh && w == ww && h == wh) { best[0] = i; dist[0] = 0; break; }
        uint32_t d = w > ww ? w - ww : ww - w;
        int cls[3] = { wh && w * wh == h * ww, w * 3 == h * 4, 1 };
        for (int k = 0; k < 3; k++) if (cls[k] && (!best[k] || d < dist[k])) { best[k] = i; dist[k] = d; }
    }
    uint32_t pick = best[0] ? best[0] : best[1] ? best[1] : best[2];
    if (ws_logging()) rt_log("ws: mode_lookup(%ux%u, %u bpp) -> record %u (%ux%u)", ww, wh, c->r[EDX], pick,
                             pick ? RD32(recs + 40 * pick) : 0, pick ? RD32(recs + 40 * pick + 4) : 0);
    if (!pick) { c->r[EAX] = ww; f_0051A530_orig(c); return; }   /* nothing usable: the game's own error */
    c->r[EAX] = pick;
    c->r[ESP] += 4;
}

/* ---- HUD (P5) ----
 * The HUD's elements (16, in 5 layouts: Options > Heads Up Display, 0x7c3d6c) have rects given
 * as fractions of the screen, {y0, x0, y1, x1} floats at 0x7cb0bc + element * 160 + layout * 32
 * (read from the HUD layout file by 0x4d0310), and 0x4d0550 turns them into pixels at +0x10:
 * x * W, y * H (W, H: the full-screen record 0x853a98/0x853a9c), clamped to the screen. On a
 * screen wider than 4:3 that stretched every element sideways. Now x is laid out on a 4:3 strip
 * (H * 4/3 wide) and each element moves with the screen edge it belongs to: the left third's to
 * the left edge, the right third's to the right, the rest stays centred. The drawing code
 * sizes the gauges, boxes and text from these rects. */
#define HUD_TABLE  0x7cb0bcu
#define SCREEN_W   0x853a98u
#define SCREEN_H   0x853a9cu
static int hud_real_w;              /* the real screen width while the game is told 4:3, else 0 */
static int screen_w(void) { return hud_real_w ? hud_real_w : (int)RD32(SCREEN_W); }
void hook_hud_layout(CPU *c) {
    int W = screen_w(), H = (int)RD32(SCREEN_H);
    if (hud_real_w) WR32(SCREEN_W, (uint32_t)W);     /* the original's clamps: the real screen */
    f_004D0550_orig(c);
    if (hud_real_w) WR32(SCREEN_W, (uint32_t)(H * 4 + 1) / 3);
    if (W * 3 <= H * 4 || H <= 0) return;
    float w43 = H * 4.0f / 3.0f, off = W - w43;
    for (int e = 0; e < 16; e++) for (int l = 0; l < 5; l++) {
        uint32_t a = HUD_TABLE + 160u * e + 32u * l;
        float x0 = RDF32(a + 4), x1 = RDF32(a + 12), cx = (x0 + x1) / 2;
        float shift = cx < 1.0f / 3 ? 0 : cx > 2.0f / 3 ? off : off / 2;
        float p0 = rintf(w43 * x0) + shift, p1 = rintf(w43 * x1) + shift;   /* fistp: round to nearest */
        if (p0 < 0) p0 = 0;
        if (p1 > W - 1) p1 = (float)(W - 1);
        WRF32(a + 16 + 4, p0); WRF32(a + 16 + 12, p1);
    }
}

/* 0x4CE720: the HUD's set-up. Scales from the screen: 0x7cb088 = W/640 (the x scale of
 * everything drawn inside the elements: text, bars, needles), 0x7cb08c = H/480 (y), 0x7cb084 =
 * W * 6/640, 0x7cb07c = H/30, 0x7cb080 = H/80, and font sizes (0x52f950: W/320). On a wide
 * screen it runs with the game told the screen is 4:3 (H * 4/3 wide), so all of those keep the
 * HUD's proportions; the element rects (hud_layout, called from it) still use the real width. */
static float hud_off;               /* W - H * 4/3 while the game is told 4:3 */
static void hud_begin(int W, int H) { hud_real_w = W; hud_off = (float)(W - (H * 4 + 1) / 3); WR32(SCREEN_W, (uint32_t)(H * 4 + 1) / 3); }
static void hud_end(void) { WR32(SCREEN_W, (uint32_t)hud_real_w); hud_real_w = 0; }
/* run f with the game told the screen is 4:3, if it's wider (and not already told) */
static void on_43_strip(CPU *c, void (*f)(CPU *)) {
    int W = (int)RD32(SCREEN_W), H = (int)RD32(SCREEN_H);
    if (hud_real_w || W * 3 <= H * 4 || H <= 0) { f(c); return; }
    hud_begin(W, H);
    f(c);
    hud_end();
}
void hook_hud_init(CPU *c) { on_43_strip(c, f_004CE720_orig); }

/* 0x4CE5E0 (race set-up, 0x4e79b0 and 0x4f73b0): the HUD's five font sizes, 0x7cb068..0x7cb078,
 * in proportion to the screen width (1.8 .. 0.75 at 960 wide). Scaled to the 4:3 strip after
 * it runs: run on the strip, its font set-up (0x52f560) would take the strip's width as the
 * screen's and drop the text of the right-hand elements. */
#define HUD_FONTS 0x7cb068u
void hook_hud_fonts(CPU *c) {
    f_004CE5E0_orig(c);
    int W = screen_w(), H = (int)RD32(SCREEN_H);
    if (W * 3 <= H * 4 || H <= 0) return;
    float k = (H * 4 + 1) / 3 / (float)W;
    for (int i = 0; i < 5; i++) WRF32(HUD_FONTS + 4u * i, RDF32(HUD_FONTS + 4u * i) * k);
}

/* 0x4CF3C0: the HUD's draw. About 25 functions under it place things straight from the screen
 * width (boxes at fractions of W, text at W/640, ...), so while it runs the game is told the
 * screen is 4:3 (0x853a98 = H * 4/3) and lays everything out on that strip; the renderer
 * (glide.c, glide_hud_*) moves what it draws: each element to its edge (hud_rect, below),
 * anything else to the centre. */
extern int glide_hud_on; extern float glide_hud_dx, glide_hud_w43;
void hook_hud_draw(CPU *c) {
    int W = (int)RD32(SCREEN_W), H = (int)RD32(SCREEN_H);
    if (W * 3 <= H * 4 || H <= 0) { f_004CF3C0_orig(c); return; }
    int w43 = (H * 4 + 1) / 3;
    hud_begin(W, H);
    glide_hud_w43 = (float)w43; glide_hud_dx = hud_off / 2; glide_hud_on = 1;
    f_004CF3C0_orig(c);
    glide_hud_on = 0;
    hud_end();
}

/* 0x4CEED0(eax = element, edx = layout, ebx = dest): copies the element's pixel rect {y0, x0,
 * y1, x1} (hud_layout: already at its edge of the real screen). Each element's draw starts
 * with it: give the element its rect on the 4:3 strip and have the renderer move what follows
 * by the element's shift. */
static float hud_shift(uint32_t element, uint32_t layout) {
    uint32_t a = HUD_TABLE + 160u * element + 32u * layout;
    float cx = (RDF32(a + 4) + RDF32(a + 12)) / 2;
    return cx < 1.0f / 3 ? 0 : cx > 2.0f / 3 ? hud_off : hud_off / 2;
}
void hook_hud_rect(CPU *c) {
    uint32_t element = c->r[EAX], layout = c->r[EDX], dest = c->r[EBX];
    f_004CEED0_orig(c);
    if (!hud_real_w || element >= 16 || layout >= 5) return;   /* (also during the HUD's init: the map's
                                                                  outline is laid out there) */
    float dx = hud_shift(element, layout);
    WRF32(dest + 4, RDF32(dest + 4) - dx); WRF32(dest + 12, RDF32(dest + 12) - dx);
    if (glide_hud_on) glide_hud_dx = dx;
}

/* Text: an element's strings are laid out by 0x52F670(eax = string, edx = x, ebx = y) into the
 * font library's own objects and all drawn at the end of the HUD by 0x52F810. So the element's
 * shift goes into x when a string is laid out, and the flush draws with none. */
/* (the font library drops glyphs past the screen width it reads: the real one for both) */
void hook_hud_text(CPU *c) {
    if (!glide_hud_on || !hud_real_w) { f_0052F670_orig(c); return; }
    c->r[EDX] += (uint32_t)(int32_t)lrintf(glide_hud_dx);
    uint32_t w43 = RD32(SCREEN_W);
    WR32(SCREEN_W, (uint32_t)hud_real_w);
    f_0052F670_orig(c);
    WR32(SCREEN_W, w43);
}
void hook_text_flush(CPU *c) {
    float dx = glide_hud_dx; glide_hud_dx = 0;
    uint32_t w = RD32(SCREEN_W);
    if (hud_real_w) WR32(SCREEN_W, (uint32_t)hud_real_w);
    f_0052F810_orig(c);
    WR32(SCREEN_W, w);
    glide_hud_dx = dx;
}

/* 0x4D09B0: drawn after the HUD, at fixed 640x480 positions times the HUD's scales: the start
 * lights. The same 4:3 strip, centred. */
void hook_hud_lights(CPU *c) {
    int W = (int)RD32(SCREEN_W), H = (int)RD32(SCREEN_H);
    if (W * 3 <= H * 4 || H <= 0) { f_004D09B0_orig(c); return; }
    int w43 = (H * 4 + 1) / 3;
    hud_begin(W, H);
    glide_hud_w43 = (float)w43; glide_hud_dx = (float)(W - w43) / 2; glide_hud_on = 1;
    f_004D09B0_orig(c);
    glide_hud_on = 0;
    hud_end();
}
