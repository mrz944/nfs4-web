/* Native display + input (SDL2), plus headless frame dumps and scripted input.
 *
 *   RT_HEADLESS=1            no window (frames dumped per RT_DUMP)
 *   RT_DUMP=N                dump every Nth presented frame to build/frames/ (0 = off)
 *   RT_INPUT="t:KEY,..."     scripted key taps at t milliseconds, e.g. "9000:RETURN,12000:DOWN"
 *                            KEY may be RETURN ESCAPE SPACE UP DOWN LEFT RIGHT TAB A..Z 0..9 F1..F12
 *                            suffix +ms to hold, e.g. "15000:UP+3000"; CLICK@x;y clicks at game coords,
 *                            MOVE@x;y only moves (both relative to the game's cursor start, see RT_MOUSE_SCALE)
 *
 * Input reaches the game three ways, because we don't know yet which one each
 * screen uses: window messages (WM_KEYDOWN/UP/CHAR, mouse), GetAsyncKeyState
 * (g_keys), and the scan codes in lParam.
 */
#define _GNU_SOURCE
#include "rt.h"
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include "gpu.h"
#if !defined(RT_NO_SDL) && !defined(__EMSCRIPTEN__)
#include <SDL2/SDL.h>
#endif

extern volatile uint8_t g_keys[256];
extern volatile int32_t g_mouse_x, g_mouse_y;
extern uint32_t g_main_hwnd;
void dinput_key(uint8_t code, int down);
void dinput_mouse_move(int dx, int dy, int dz);
void dinput_mouse_button(int b, int down);

#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_CLOSE 0x0010

/* ------------------------------------------------------------ key tables */
typedef struct { const char *name; uint8_t vk, scan, ext; char ch; } Key;
static const Key keys[] = {
    {"ESCAPE", 0x1B, 0x01, 0, 27}, {"RETURN", 0x0D, 0x1C, 0, 13}, {"SPACE", 0x20, 0x39, 0, ' '},
    {"TAB", 0x09, 0x0F, 0, 9}, {"BACKSPACE", 0x08, 0x0E, 0, 8},
    {"UP", 0x26, 0x48, 1, 0}, {"DOWN", 0x28, 0x50, 1, 0}, {"LEFT", 0x25, 0x4B, 1, 0}, {"RIGHT", 0x27, 0x4D, 1, 0},
    {"LSHIFT", 0x10, 0x2A, 0, 0}, {"RSHIFT", 0x10, 0x36, 0, 0}, {"LCTRL", 0x11, 0x1D, 0, 0}, {"RCTRL", 0x11, 0x1D, 1, 0},
    {"LALT", 0x12, 0x38, 0, 0}, {"RALT", 0x12, 0x38, 1, 0},
    {"INSERT", 0x2D, 0x52, 1, 0}, {"DELETE", 0x2E, 0x53, 1, 0}, {"HOME", 0x24, 0x47, 1, 0}, {"END", 0x23, 0x4F, 1, 0},
    {"PAGEUP", 0x21, 0x49, 1, 0}, {"PAGEDOWN", 0x22, 0x51, 1, 0},
    {"F1", 0x70, 0x3B, 0, 0}, {"F2", 0x71, 0x3C, 0, 0}, {"F3", 0x72, 0x3D, 0, 0}, {"F4", 0x73, 0x3E, 0, 0},
    {"F5", 0x74, 0x3F, 0, 0}, {"F6", 0x75, 0x40, 0, 0}, {"F7", 0x76, 0x41, 0, 0}, {"F8", 0x77, 0x42, 0, 0},
    {"F9", 0x78, 0x43, 0, 0}, {"F10", 0x79, 0x44, 0, 0}, {"F11", 0x7A, 0x57, 0, 0}, {"F12", 0x7B, 0x58, 0, 0},
    {"A", 'A', 0x1E, 0, 'a'}, {"B", 'B', 0x30, 0, 'b'}, {"C", 'C', 0x2E, 0, 'c'}, {"D", 'D', 0x20, 0, 'd'},
    {"E", 'E', 0x12, 0, 'e'}, {"F", 'F', 0x21, 0, 'f'}, {"G", 'G', 0x22, 0, 'g'}, {"H", 'H', 0x23, 0, 'h'},
    {"I", 'I', 0x17, 0, 'i'}, {"J", 'J', 0x24, 0, 'j'}, {"K", 'K', 0x25, 0, 'k'}, {"L", 'L', 0x26, 0, 'l'},
    {"M", 'M', 0x32, 0, 'm'}, {"N", 'N', 0x31, 0, 'n'}, {"O", 'O', 0x18, 0, 'o'}, {"P", 'P', 0x19, 0, 'p'},
    {"Q", 'Q', 0x10, 0, 'q'}, {"R", 'R', 0x13, 0, 'r'}, {"S", 'S', 0x1F, 0, 's'}, {"T", 'T', 0x14, 0, 't'},
    {"U", 'U', 0x16, 0, 'u'}, {"V", 'V', 0x2F, 0, 'v'}, {"W", 'W', 0x11, 0, 'w'}, {"X", 'X', 0x2D, 0, 'x'},
    {"Y", 'Y', 0x15, 0, 'y'}, {"Z", 'Z', 0x2C, 0, 'z'},
    {"1", '1', 0x02, 0, '1'}, {"2", '2', 0x03, 0, '2'}, {"3", '3', 0x04, 0, '3'}, {"4", '4', 0x05, 0, '4'},
    {"5", '5', 0x06, 0, '5'}, {"6", '6', 0x07, 0, '6'}, {"7", '7', 0x08, 0, '7'}, {"8", '8', 0x09, 0, '8'},
    {"9", '9', 0x0A, 0, '9'}, {"0", '0', 0x0B, 0, '0'},
};
#define NKEYS (sizeof keys / sizeof *keys)

static const Key *key_by_name(const char *n) {
    for (size_t i = 0; i < NKEYS; i++) if (!strcasecmp(keys[i].name, n)) return &keys[i];
    return NULL;
}

/* Windows VK side-specific codes, also set so either style of check works */
static uint8_t vk_side(const Key *k) {
    if (!strcmp(k->name, "LSHIFT")) return 0xA0; if (!strcmp(k->name, "RSHIFT")) return 0xA1;
    if (!strcmp(k->name, "LCTRL")) return 0xA2;  if (!strcmp(k->name, "RCTRL")) return 0xA3;
    if (!strcmp(k->name, "LALT")) return 0xA4;   if (!strcmp(k->name, "RALT")) return 0xA5;
    return 0;
}

void input_key(const Key *k, int down) {
    dinput_key((uint8_t)(k->scan | (k->ext ? 0x80 : 0)), down);
    int was = g_keys[k->vk];
    g_keys[k->vk] = (uint8_t)down;
    uint8_t s = vk_side(k); if (s) g_keys[s] = (uint8_t)down;
    if (!g_main_hwnd) return;
    int alt = k->vk == 0x12;
    uint32_t lp = 1u | ((uint32_t)k->scan << 16) | ((uint32_t)k->ext << 24);
    if (!down) lp |= 0xC0000000u;
    else if (was) lp |= 0x40000000u;     /* auto-repeat */
    msg_post(g_main_hwnd, down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP), k->vk, lp);
    if (down && k->ch) msg_post(g_main_hwnd, WM_CHAR, (uint8_t)k->ch, lp);
}
/* RT_CURSOR_ADDR=hex: the game's own cursor x,y (two int32s; NFS4: 689510). The game moves its
 * cursor to the focused button by itself, so scripted moves start from where it really is. */
static void sync_cursor(void) {
    const char *e = getenv("RT_CURSOR_ADDR"); if (!e) return;
    uint32_t a = (uint32_t)strtoul(e, NULL, 16);
    g_mouse_x = (int32_t)RD32(a); g_mouse_y = (int32_t)RD32(a + 4);
}
static void input_mouse(int x, int y, int button, int down) {
    if (button == 0) dinput_mouse_move(x - g_mouse_x, y - g_mouse_y, 0);
    else dinput_mouse_button(button == 1 ? 0 : button == 3 ? 1 : 2, down);
    g_mouse_x = x; g_mouse_y = y;
    if (!g_main_hwnd) return;
    uint32_t lp = ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF);
    if (button == 0) msg_post(g_main_hwnd, WM_MOUSEMOVE, 0, lp);
    else if (button == 1) { g_keys[0x01] = (uint8_t)down; msg_post(g_main_hwnd, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? 1 : 0, lp); }
    else if (button == 3) { g_keys[0x02] = (uint8_t)down; msg_post(g_main_hwnd, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? 2 : 0, lp); }
}

/* --------------------------------------------------------- scripted input */
static uint64_t ms_since_start(void) {
    static struct timespec t0; static int init;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    if (!init) { t0 = t; init = 1; }
    return (uint64_t)(t.tv_sec - t0.tv_sec) * 1000 + (uint64_t)((t.tv_nsec - t0.tv_nsec) / 1000000);
}
static void sleep_until_ms(uint64_t ms) {
    for (uint64_t now; (now = ms_since_start()) < ms;) usleep((useconds_t)((ms - now) * 1000));
}
static void *script_main(void *p) {
    char *spec = p;
    /* RT_MOUSE_START=x;y: where the game's own cursor starts, so CLICK@/MOVE@ coordinates are
     * absolute screen positions (NFS4 parks it on the focused button, ~85;465 on the Load screen) */
    if (getenv("RT_MOUSE_START")) { const char *m = getenv("RT_MOUSE_START"); g_mouse_x = atoi(m); g_mouse_y = strchr(m, ';') ? atoi(strchr(m, ';') + 1) : 0; }
    for (char *tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
        uint64_t t = strtoull(tok, NULL, 10);
        char *k = strchr(tok, ':'); if (!k) continue;
        k++;
        uint32_t hold = 120;
        char *plus = strchr(k, '+'); if (plus) { *plus = 0; hold = (uint32_t)atoi(plus + 1); }
        if (!strncasecmp(k, "MEMDUMP", 7)) {        /* MEMDUMP: write the exe's data+bss to build/mem_<t>.bin (RE aid) */
            sleep_until_ms(t);
            char p[64]; snprintf(p, sizeof p, "build/mem_%llu.bin", (unsigned long long)t);
            FILE *f = fopen(p, "wb"); if (f) { fwrite(G + 0x591000, 1, 0xA3B000 - 0x591000, f); fclose(f); }
            rt_log("input: memory dump %s", p);
            continue;
        }
        if (!strncasecmp(k, "MOVE@", 5)) {           /* MOVE@x;y: move only (same coordinates as CLICK) */
            int x = atoi(k + 5), y = 0; char *semi = strchr(k, ';'); if (semi) y = atoi(semi + 1);
            sleep_until_ms(t);
            sync_cursor();
            int x0 = g_mouse_x, y0 = g_mouse_y;
            for (int i = 1; i <= 4; i++) { input_mouse(x0 + (x - x0) * i / 4, y0 + (y - y0) * i / 4, 0, 0); usleep(30000); }
            continue;
        }
        if (!strncasecmp(k, "CLICK@", 6)) {          /* CLICK@x;y in 640x480 game coordinates */
            int x = atoi(k + 6), y = 0; char *semi = strchr(k, ';'); if (semi) y = atoi(semi + 1);
            sleep_until_ms(t);
            rt_log("input: click %d,%d (t=%llums)", x, y, (unsigned long long)t);
            /* the game's cursor is relative (DirectInput) and unclamped: move by the
             * difference from where we last put it (starts at the game's own default) */
            sync_cursor();
            int x0 = g_mouse_x, y0 = g_mouse_y;
            for (int i = 1; i <= 4; i++) { input_mouse(x0 + (x - x0) * i / 4, y0 + (y - y0) * i / 4, 0, 0); usleep(30000); }
            input_mouse(x, y, 1, 1); usleep(hold * 1000); input_mouse(x, y, 1, 0);
            continue;
        }
        const Key *key = key_by_name(k);
        if (!key) { rt_log("RT_INPUT: unknown key %s", k); continue; }
        sleep_until_ms(t);
        rt_log("input: %s down (t=%llums)", key->name, (unsigned long long)t);
        input_key(key, 1);
        usleep(hold * 1000);
        input_key(key, 0);
    }
    return NULL;
}

/* ---------------------------------------------------------------- frames */
static int dump_every = -1;
static uint32_t frame_no;

static void dump_ppm(const uint8_t *px, int w, int h, int pitch, int bpp, uint32_t n) {
    const char *dir = getenv("RT_DUMP_DIR") ? getenv("RT_DUMP_DIR") : "build/frames";
    mkdir(dir, 0755);
    char path[512]; snprintf(path, sizeof path, "%s/%05u.ppm", dir, n);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t rgb[3];
        if (bpp == 16) { uint16_t p; memcpy(&p, px + y * pitch + x * 2, 2);
            rgb[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31); rgb[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63); rgb[2] = (uint8_t)((p & 31) * 255 / 31); }
        else { uint32_t p; memcpy(&p, px + y * pitch + x * 4, 4); rgb[0] = (uint8_t)(p >> 16); rgb[1] = (uint8_t)(p >> 8); rgb[2] = (uint8_t)p; }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

#ifdef __EMSCRIPTEN__
/* ------------------------------------------------------- browser platform
 * The page (web/main.js) shares wasm memory with us:
 *   rt_web_fb()     framebuffer + sequence number, read every animation frame
 *   rt_web_input()  lock-free SPSC ring of input events written by the page
 *   rt_web_key_index(name) maps the page's key names to our key table
 * Audio has its own ring in dsound.c (rt_web_audio). */
#include <emscripten.h>
typedef struct { volatile uint32_t seq; uint32_t w, h, bpp, pitch; uint8_t px[1024 * 768 * 4]; } WebFB;
static WebFB web_fb;
EMSCRIPTEN_KEEPALIVE void *rt_web_fb(void) { return &web_fb; }

enum { EV_KEY = 1, EV_MOVE = 2, EV_BUTTON = 3, EV_WHEEL = 4 };
#define WEB_QN 1024
typedef struct { volatile uint32_t head, tail; int32_t ev[WEB_QN][4]; } WebInput;
static WebInput web_in;
EMSCRIPTEN_KEEPALIVE void *rt_web_input(void) { return &web_in; }
EMSCRIPTEN_KEEPALIVE int rt_web_key_index(const char *name) {
    const Key *k = key_by_name(name);
    return k ? (int)(k - keys) : -1;
}

static void web_publish(uint32_t mem, int w, int h, int pitch, int bpp) {
    size_t sz = (size_t)pitch * h;
    if (sz > sizeof web_fb.px) return;
    memcpy(web_fb.px, G + mem, sz);
    web_fb.w = (uint32_t)w; web_fb.h = (uint32_t)h; web_fb.bpp = (uint32_t)bpp; web_fb.pitch = (uint32_t)pitch;
    __atomic_add_fetch(&web_fb.seq, 1, __ATOMIC_RELEASE);
}

static void *web_input_main(void *unused) {
    (void)unused;
    for (;;) {
        uint32_t head = __atomic_load_n(&web_in.head, __ATOMIC_ACQUIRE);
        while (web_in.tail != head) {
            int32_t *e = web_in.ev[web_in.tail % WEB_QN];
            switch (e[0]) {
            case EV_KEY: if (e[1] >= 0 && (size_t)e[1] < NKEYS) input_key(&keys[e[1]], e[2]); break;
            case EV_MOVE:
                dinput_mouse_move(e[1], e[2], 0);
                g_mouse_x += e[1]; g_mouse_y += e[2];
                break;
            case EV_BUTTON: input_mouse(g_mouse_x, g_mouse_y, e[1], e[2]); break;
            case EV_WHEEL: dinput_mouse_move(0, 0, e[1]); break;
            }
            __atomic_store_n(&web_in.tail, web_in.tail + 1, __ATOMIC_RELEASE);
        }
        usleep(2000);
    }
    return NULL;
}
#endif

/* latest frame, handed from the game's presenting thread to the display thread */
static pthread_mutex_t fb_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *fb; static int fb_w, fb_h, fb_pitch, fb_bpp, fb_new;
static int use_window;

void present_frame(uint32_t mem, int w, int h, int pitch, int bpp) {
    uint32_t n = ++frame_no;
    if (dump_every > 0 && (n % (uint32_t)dump_every == 0 || n <= 3)) dump_ppm(G + mem, w, h, pitch, bpp, n);
    if (n == 1 || n % 600 == 0) { extern uint32_t heap_used(void); rt_log("frame %u (%dx%d %dbpp) guest heap %u MB", n, w, h, bpp, heap_used() >> 20); }
#ifdef __EMSCRIPTEN__
    gpu_ddraw_frame(G + mem, w, h, pitch, bpp);
    return;
#endif
    if (!use_window) return;
    gpu_ddraw_frame(G + mem, w, h, pitch, bpp);
}

/* ------------------------------------------------------------------ SDL */
#if !defined(RT_NO_SDL) && !defined(__EMSCRIPTEN__)
static const Key *key_from_sdl(SDL_Scancode sc) {
    const char *n = NULL;
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: n = "ESCAPE"; break; case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: n = "RETURN"; break;
    case SDL_SCANCODE_SPACE: n = "SPACE"; break; case SDL_SCANCODE_TAB: n = "TAB"; break;
    case SDL_SCANCODE_BACKSPACE: n = "BACKSPACE"; break;
    case SDL_SCANCODE_UP: n = "UP"; break; case SDL_SCANCODE_DOWN: n = "DOWN"; break;
    case SDL_SCANCODE_LEFT: n = "LEFT"; break; case SDL_SCANCODE_RIGHT: n = "RIGHT"; break;
    case SDL_SCANCODE_LSHIFT: n = "LSHIFT"; break; case SDL_SCANCODE_RSHIFT: n = "RSHIFT"; break;
    case SDL_SCANCODE_LCTRL: n = "LCTRL"; break; case SDL_SCANCODE_RCTRL: n = "RCTRL"; break;
    case SDL_SCANCODE_LALT: n = "LALT"; break; case SDL_SCANCODE_RALT: n = "RALT"; break;
    case SDL_SCANCODE_INSERT: n = "INSERT"; break; case SDL_SCANCODE_DELETE: n = "DELETE"; break;
    case SDL_SCANCODE_HOME: n = "HOME"; break; case SDL_SCANCODE_END: n = "END"; break;
    case SDL_SCANCODE_PAGEUP: n = "PAGEUP"; break; case SDL_SCANCODE_PAGEDOWN: n = "PAGEDOWN"; break;
    default:
        if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { static char b[2]; b[0] = (char)('A' + sc - SDL_SCANCODE_A); b[1] = 0; n = b; }
        else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { static char b[2]; b[0] = (char)('1' + sc - SDL_SCANCODE_1); b[1] = 0; n = b; }
        else if (sc == SDL_SCANCODE_0) n = "0";
        else if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) { static char b[4]; snprintf(b, sizeof b, "F%d", 1 + sc - SDL_SCANCODE_F1); n = b; }
    }
    return n ? key_by_name(n) : NULL;
}

static SDL_Window *win;
static SDL_GLContext glctx;

void display_loop(void) {
    SDL_GL_MakeCurrent(win, glctx);
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { rt_log("window closed"); if (g_main_hwnd) msg_post(g_main_hwnd, WM_CLOSE, 0, 0); _exit(0); }
            if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)) {
                const Key *k = key_from_sdl(e.key.keysym.scancode);
                if (k) input_key(k, e.type == SDL_KEYDOWN);
            }
            if (e.type == SDL_MOUSEMOTION) {
                dinput_mouse_move(e.motion.xrel, e.motion.yrel, 0);
                g_mouse_x = e.motion.x; g_mouse_y = e.motion.y;
                if (g_main_hwnd) msg_post(g_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(e.motion.y & 0xFFFF) << 16) | (uint32_t)(e.motion.x & 0xFFFF));
            }
            if (e.type == SDL_MOUSEWHEEL) dinput_mouse_move(0, 0, e.wheel.y * 120);
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP)
                input_mouse(e.button.x, e.button.y, e.button.button, e.type == SDL_MOUSEBUTTONDOWN);
        }
        int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
        gpu_render(w, h);
        SDL_GL_SwapWindow(win);   /* vsync paces this loop */
    }
}

#include <dirent.h>
/* Shells started outside the desktop session (IDE terminals, agents, ssh)
 * often lack WAYLAND_DISPLAY/DISPLAY even though a compositor is running. */
static void find_display(void) {
    if (getenv("WAYLAND_DISPLAY") || getenv("DISPLAY")) return;
    const char *rd = getenv("XDG_RUNTIME_DIR");
    DIR *d = rd ? opendir(rd) : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, "wayland-", 8) && !strstr(e->d_name, ".lock")) {
                setenv("WAYLAND_DISPLAY", e->d_name, 1);
                rt_log("WAYLAND_DISPLAY was unset; using %s/%s", rd, e->d_name);
                break;
            }
        closedir(d);
    }
    if (!getenv("DISPLAY") && (d = opendir("/tmp/.X11-unix"))) {
        struct dirent *e; int best = -1;
        while ((e = readdir(d))) if (e->d_name[0] == 'X' && isdigit((unsigned char)e->d_name[1]) && !strchr(e->d_name, '_')) {
            int n = atoi(e->d_name + 1); if (best < 0 || n < best) best = n; }
        closedir(d);
        if (best >= 0) { char v[16]; snprintf(v, sizeof v, ":%d", best); setenv("DISPLAY", v, 1); rt_log("DISPLAY was unset; using %s", v); }
    }
}

static int open_window_once(void);
static int open_window(void) {
    find_display();
    if (open_window_once()) return 1;
    /* Wayland trouble (compositor, missing libdecor, ...): retry via XWayland */
    const char *drv = getenv("SDL_VIDEODRIVER");
    if (drv && !strcmp(drv, "x11")) return 0;
    rt_log("retrying with SDL_VIDEODRIVER=x11 (XWayland)");
    if (glctx) { SDL_GL_DeleteContext(glctx); glctx = NULL; }
    if (win) { SDL_DestroyWindow(win); win = NULL; }
    SDL_Quit();
    setenv("SDL_VIDEODRIVER", "x11", 1);
    return open_window_once();
}
static int open_window_once(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { rt_log("SDL_Init failed: %s", SDL_GetError()); return 0; }
    rt_log("SDL video driver: %s", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win = SDL_CreateWindow("Need for Speed III (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { rt_log("SDL_CreateWindow failed: %s", SDL_GetError()); return 0; }
    glctx = SDL_GL_CreateContext(win);
    if (!glctx) { rt_log("SDL_GL_CreateContext (GLES 3.0) failed: %s", SDL_GetError()); return 0; }
    SDL_GL_SetSwapInterval(1);
    rt_log("window open (GL ES 3 context)");
    SDL_GL_MakeCurrent(win, NULL);   /* the display loop takes it */
    return 1;
}
#else
void display_loop(void) { for (;;) pause(); }
#endif

int display_init(void) {
    const char *d = getenv("RT_DUMP");
    int headless = getenv("RT_HEADLESS") != NULL;
    dump_every = d ? atoi(d) : -1;
    ms_since_start();
#ifdef __EMSCRIPTEN__
    (void)headless;
    dump_every = 0;
    { pthread_t t; pthread_create(&t, NULL, web_input_main, NULL); }
#elif !defined(RT_NO_SDL)
    if (!headless) use_window = open_window();
#endif
    if (!use_window && dump_every < 0) dump_every = 30;
    const char *in = getenv("RT_INPUT");
    if (in) { pthread_t t; pthread_create(&t, NULL, script_main, strdup(in)); }
    return use_window;
}
