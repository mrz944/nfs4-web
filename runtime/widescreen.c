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
