/* Native headless GPU rendering for tests: with RT_HEADLESS=1 RT_DUMP=N the
 * Glide frames are rendered offscreen (EGL, surfaceless Mesa) on the game
 * thread and every Nth one is written to build/frames/gNNNNN.ppm.
 * RT_DUMP_H=H: the dump's height (default 480), its width follows the frame's
 * aspect (a 1280x720 race dumps 853x480); RT_DUMP_H=0: the frame's own size. */
#if !defined(__EMSCRIPTEN__) && !defined(RT_NO_SDL)
#define _GNU_SOURCE
#include "rt.h"
#include "gpu.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

extern GLuint gpu_target_fbo;
extern volatile double gpu_renderer_seen_reset;
void gpu_headless_frame(GFrame *unused) {
    (void)unused;
    static int every = -1, n;
    if (every < 0) {
        every = 0;
        if (getenv("RT_HEADLESS") && getenv("RT_DUMP")) every = atoi(getenv("RT_DUMP"));
        if (every <= 0) every = 0;
    }
    if (!every || ++n % every) return;
    static EGLDisplay dpy; static int ok = -1; static GLuint fb, rb; static int fw, fh;
    if (ok < 0) {
        ok = 0;
        PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!dpy || !eglInitialize(dpy, NULL, NULL)) { rt_log("gpu headless: no EGL display"); return; }
        eglBindAPI(EGL_OPENGL_ES_API);
        rt_log("gpu headless: EGL %s (%s)", eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
        EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE }; EGLConfig cfg = NULL; EGLint nc = 0;
        if (!eglChooseConfig(dpy, ca, &cfg, 1, &nc) || !nc) cfg = NULL;   /* EGL_KHR_no_config_context */
        EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
        if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { rt_log("gpu headless: no EGL context"); return; }
        glGenFramebuffers(1, &fb); glGenRenderbuffers(1, &rb);
        ok = 1;
    }
    if (!ok) return;
    static int dump_h = -1;
    if (dump_h < 0) { const char *e = getenv("RT_DUMP_H"); dump_h = e ? atoi(e) : 480; if (dump_h < 0) dump_h = 480; }
    gpu_target_fbo = fb;
    /* draw, then if the frame's size wants another dump size, resize and draw again (a redraw
       of the same frame only blits it) */
    for (int pass = 0; pass < 2; pass++) {
        if (fw) gpu_render(fw, fh);
        int sw, sh; gpu_last_source(&sw, &sh);
        int w = dump_h ? (int)((double)dump_h * sw / sh + 0.5) : sw, h = dump_h ? dump_h : sh;
        if (fw && w == fw && h == fh) break;
        fw = w; fh = h;
        glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, fw, fh);
        glBindFramebuffer(GL_FRAMEBUFFER, fb); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        if (pass) gpu_render(fw, fh);
    }
    gpu_renderer_seen_reset = 1;
    uint8_t *px = malloc((size_t)fw * fh * 4); if (!px) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const char *dir = getenv("RT_DUMP_DIR") ? getenv("RT_DUMP_DIR") : "build/frames";
    char path[512]; snprintf(path, sizeof path, "%s/g%06d.ppm", dir, n);
    FILE *f = fopen(path, "wb"); if (!f) { free(px); return; }
    fprintf(f, "P6\n%d %d\n255\n", fw, fh);
    for (int y = fh - 1; y >= 0; y--) for (int x = 0; x < fw; x++) fwrite(px + ((size_t)y * fw + x) * 4, 1, 3, f);
    fclose(f); free(px);
}
#endif
