/* Widescreen patch (docs/widescreen-plan.md): hooks on the game's view, projection and HUD code
 * (runtime/hooks.txt). 4:3 frames keep the original code path.
 *   RT_WS_LOG=1   log the hooks' arguments (each distinct set once, up to 64) */
#include "hooks.h"

static int ws_log = -1;
static int ws_logging(void) {
    if (ws_log < 0) { const char *e = getenv("RT_WS_LOG"); ws_log = e && *e && *e != '0' ? 64 : 0; }
    return ws_log > 0;
}

/* 0x453F10: view setup (x, y, w, h, fov, n, flip), floats except flip, all on the stack, ret 0x1c.
 * Sets the half sizes 0x5b0958/0x5b095c = w/2, h/2, the centre 0x680f98/0x680f9c = x + w/2,
 * y + h/2, and the focal length 0x5b0950 = (w / tan(fov * [0x5938d1])) / 2 (w/2 when fov is 0),
 * 0x5b0954 = -focal (the two swapped when flip). Focal from the width: a fixed horizontal FOV. */
void hook_view_setup(CPU *c) {
    if (ws_logging()) {
        static uint32_t seen[64][8]; static int nseen;
        uint32_t a[8];
        for (int i = 0; i < 7; i++) a[i] = HOOK_ARG32(c, i);
        a[7] = RD32(c->r[ESP]);
        int k = 0; while (k < nseen && memcmp(seen[k], a, sizeof a)) k++;
        if (k == nseen && nseen < 64) {
            memcpy(seen[nseen++], a, sizeof a); ws_log--;
            float x, y, w, h, fov, n; memcpy(&x, &a[0], 4); memcpy(&y, &a[1], 4); memcpy(&w, &a[2], 4);
            memcpy(&h, &a[3], 4); memcpy(&fov, &a[4], 4); memcpy(&n, &a[5], 4);
            rt_log("ws: view_setup(x %g, y %g, w %g, h %g, fov %g, n %g, flip %u) from 0x%x",
                   x, y, w, h, fov, n, a[6], a[7]);
        }
    }
    f_00453F10_orig(c);
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
