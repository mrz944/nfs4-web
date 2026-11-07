/* GL ES 3 / WebGL2 renderer for frames recorded by glide.c (see gpu.h), plus
 * presentation of software (DirectDraw) frames such as movies.
 *
 * Runs on the display thread: SDL's main thread natively, the page's main
 * thread in the browser (rt_gpu_frame, called from requestAnimationFrame).
 */
#define _GNU_SOURCE
#include "rt.h"
#include "gpu.h"
#include <pthread.h>
#include <time.h>
#include <math.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#elif !defined(RT_NO_SDL)
#include <GLES3/gl3.h>
#endif

int g_gpu_active;

/* ------------------------------------------------- frames (any thread) */
GFrame *gpu_frame_new(void) { return calloc(1, sizeof(GFrame)); }
void gpu_frame_free(GFrame *f) {
    if (!f) return;
    for (uint32_t i = 0; i < f->nlfbs; i++) free(f->lfbs[i].rgba);
    free(f->cmds); free(f->verts); free(f->states); free(f->lfbs); free(f);
}

/* texture table */
static pthread_mutex_t tex_mu = PTHREAD_MUTEX_INITIALIZER;
static GTex texs[GPU_MAX_TEX];
static int ntex_hi;
void gpu_tex_lock(void) { pthread_mutex_lock(&tex_mu); }
void gpu_tex_unlock(void) { pthread_mutex_unlock(&tex_mu); }
GTex *gpu_tex(int slot) { if (slot + 1 > ntex_hi) ntex_hi = slot + 1; return &texs[slot]; }
int gpu_tex_count(void) { return ntex_hi; }

static pthread_mutex_t fq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fq_cv = PTHREAD_COND_INITIALIZER;
static GFrame *pending;                 /* newest complete frame not yet taken by the renderer */
static uint32_t frame_seq;
/* the game's frames stop for 5 s: log what every thread is doing (then every 10 s while it lasts) */
static void *stall_watch(void *unused) {
    (void)unused;
    uint32_t seen = 0; int quiet = 0, next = 5, secs = 0;
    const char *at = getenv("RT_WATCH_AT");                  /* debugging: also dump at this second */
    for (;;) {
        nanosleep(&(struct timespec){ 1, 0 }, NULL);
        if (at && ++secs == atoi(at)) rt_watch_dump("requested");
        uint32_t f = __atomic_load_n(&frame_seq, __ATOMIC_RELAXED);
        if (f != seen) { seen = f; quiet = 0; next = 5; continue; }
        if (++quiet == next) { char why[48]; snprintf(why, sizeof why, "no frame for %d s after frame %u", quiet, f); rt_watch_dump(why); next += 10; }
    }
    return NULL;
}
void gpu_start_watchdog(void) { static int on; if (!on) { on = 1; pthread_t t; pthread_create(&t, NULL, stall_watch, NULL); } }
static volatile double renderer_seen;   /* last time the renderer ran (seconds) */
volatile double gpu_renderer_seen_reset; /* headless test renderer: don't pace the game to it */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

static volatile double last_gl_frame, last_dd_frame;
/* pacing counters for the page's perf log (rt_gpu_stats): frames the game published, frames
 * replaced before the renderer showed them, and time the game spent waiting for the renderer */
static double st_published, st_replaced, st_wait_ms;

/* does this frame start by clearing the whole picture? */
static int frame_is_full(const GFrame *f) {
    for (uint32_t i = 0; i < f->ncmds; i++) {
        const GCmd *c = &f->cmds[i];
        if (c->type == GC_CLEAR) return c->clear_color_on && c->clip[0] <= 0 && c->clip[1] <= 0 && c->clip[2] >= f->w && c->clip[3] >= f->h;
        if (c->type == GC_DRAW || c->type == GC_LFB) return 0;
    }
    return 0;
}
static void frame_append(GFrame *d, GFrame *s) {
    uint32_t vo = d->nverts, so = d->nstates, lo = d->nlfbs;
    if (d->nverts + s->nverts > d->capverts) { d->capverts = d->nverts + s->nverts + 1024; d->verts = realloc(d->verts, d->capverts * sizeof(GVert)); }
    memcpy(d->verts + d->nverts, s->verts, s->nverts * sizeof(GVert)); d->nverts += s->nverts;
    if (d->nstates + s->nstates > d->capstates) { d->capstates = d->nstates + s->nstates + 64; d->states = realloc(d->states, d->capstates * sizeof(GState)); }
    memcpy(d->states + d->nstates, s->states, s->nstates * sizeof(GState)); d->nstates += s->nstates;
    if (d->nlfbs + s->nlfbs > d->caplfbs) { d->caplfbs = d->nlfbs + s->nlfbs + 4; d->lfbs = realloc(d->lfbs, d->caplfbs * sizeof(GLfb)); }
    memcpy(d->lfbs + d->nlfbs, s->lfbs, s->nlfbs * sizeof(GLfb)); d->nlfbs += s->nlfbs; s->nlfbs = 0;   /* buffers move */
    if (d->ncmds + s->ncmds > d->capcmds) { d->capcmds = d->ncmds + s->ncmds + 64; d->cmds = realloc(d->cmds, d->capcmds * sizeof(GCmd)); }
    for (uint32_t i = 0; i < s->ncmds; i++) {
        GCmd c = s->cmds[i];
        if (c.type == GC_DRAW) { c.state += so; c.first += vo; } else if (c.type == GC_LFB) c.lfb += lo;
        d->cmds[d->ncmds++] = c;
    }
    memcpy(d->fog_lut, s->fog_lut, sizeof d->fog_lut);
}

void gpu_publish(GFrame *f) {
    g_gpu_active = 1;
    extern void gpu_headless_frame(GFrame *f);
    pthread_mutex_lock(&fq_mu);
    f->seq = ++frame_seq;
    if (f->seq % 300 == 1) {
        uint32_t draws = 0, clears = 0; for (uint32_t i = 0; i < f->ncmds; i++) { draws += f->cmds[i].type == GC_DRAW; clears += f->cmds[i].type == GC_CLEAR; }
        rt_log("gpu frame %u: %u tris, %u draws, %u states, %u clears, %u lfb, %d textures", f->seq, f->nverts / 3, draws, f->nstates, clears, f->nlfbs, ntex_hi);
    }
    /* pace the game to the display: wait (briefly) for the renderer to take the previous frame */
    if (gpu_renderer_seen_reset) renderer_seen = 0;
    if (pending && now_s() - renderer_seen < 0.5) {
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 25 * 1000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
        double w0 = now_s();
        while (pending && pthread_cond_timedwait(&fq_cv, &fq_mu, &dl) == 0) {}
        st_wait_ms += (now_s() - w0) * 1000;
    }
#ifdef __EMSCRIPTEN__
    /* nobody is drawing (hidden tab/window: the browser stops animation frames):
     * don't let the game spin out hundreds of thousands of frames per second */
    if (now_s() - renderer_seen >= 0.5) {
        static double last_pub;
        double wait = last_pub + 1.0 / 60 - now_s();
        if (wait > 0) { pthread_mutex_unlock(&fq_mu); struct timespec ts = { 0, (long)(wait * 1e9) }; nanosleep(&ts, NULL); pthread_mutex_lock(&fq_mu); }
        last_pub = now_s();
    }
#endif
    GFrame *old = NULL;
    /* merged incremental frames must not grow without bound while nobody takes them */
    if (pending && (pending->nverts > 300000 || pending->ncmds > 20000 || pending->nlfbs > 8)) { old = pending; pending = NULL; gpu_frame_free(old); old = NULL; }
    if (pending && !frame_is_full(f)) {
        /* incremental frame (draws over the previous picture, e.g. a loading bar):
         * append it to the frame still waiting so nothing it builds on is lost */
        frame_append(pending, f); old = f; pending->seq = f->seq;
    } else { if (pending) st_replaced++; old = pending; pending = f; }
    st_published++;
    last_gl_frame = now_s();
    pthread_mutex_unlock(&fq_mu);
    gpu_frame_free(old);
#if !defined(__EMSCRIPTEN__)
    gpu_headless_frame(NULL);
#endif
}

/* software frames */
static pthread_mutex_t dd_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *dd_px; static int dd_w, dd_h, dd_pitch, dd_bpp, dd_new;
void gpu_ddraw_frame(const uint8_t *px, int w, int h, int pitch, int bpp) {
    pthread_mutex_lock(&dd_mu);
    size_t sz = (size_t)pitch * h;
    if (!dd_px || dd_pitch * dd_h != (int)sz) { free(dd_px); dd_px = malloc(sz); }
    memcpy(dd_px, px, sz);
    dd_w = w; dd_h = h; dd_pitch = pitch; dd_bpp = bpp; dd_new = 1;
    last_dd_frame = now_s();
    pthread_mutex_unlock(&dd_mu);
}

#if defined(__EMSCRIPTEN__) || !defined(RT_NO_SDL)
/* ------------------------------------------------------------ renderer */
static GLuint prog, prog_blit, vbo, vao, quad_vao, quad_vbo, fog_tex, ovl_tex, dd_tex;
static GLuint fbo, fbo_color, fbo_depth; static int fbo_w, fbo_h;
static GLuint gltex[GPU_MAX_TEX]; static uint32_t gltex_ver[GPU_MAX_TEX];
static uint32_t gltex_par[GPU_MAX_TEX];  /* filter/wrap last set on each texture (+1), to skip repeats */
static int max_scale = 4;               /* upper limit for the integer render scale (rt_gpu_set_max_scale) */
static GFrame *shown;
static GLint u_res, u_has_tex, u_cc, u_ac, u_tc, u_inv, u_const, u_fogc, u_chroma, u_atest, u_aref, u_fog_on, u_chroma_on;
static GLint ub_swz, ub_rect, ub_key;
GLuint gpu_target_fbo;                  /* where the finished picture goes (0 = window) */

static const char *VS =
    "#version 300 es\n"
    "layout(location=0) in vec4 a_pos; layout(location=1) in vec4 a_col; layout(location=2) in vec2 a_st;\n"
    "uniform vec2 u_res;\n"
    "out vec4 v_colw; out float v_w; out vec2 v_st;\n"
    "void main() {\n"
    "  float w = a_pos.w;\n"
    "  vec2 ndc = vec2(a_pos.x / u_res.x * 2.0 - 1.0, 1.0 - a_pos.y / u_res.y * 2.0);\n"
    "  gl_Position = vec4(ndc * w, (a_pos.z * 2.0 - 1.0) * w, w);\n"
    "  v_colw = a_col * w; v_w = w; v_st = a_st;\n"   /* colour: screen-linear via c*w / w */
    "}\n";
static const char *FS =
    "#version 300 es\n"
    "precision highp float; precision highp int;\n"
    "in vec4 v_colw; in float v_w; in vec2 v_st;\n"
    "uniform sampler2D u_tex; uniform sampler2D u_fog;\n"
    "uniform bool u_has_tex; uniform ivec4 u_cc, u_ac, u_tc, u_inv;\n"
    "uniform vec4 u_const, u_fogc, u_chroma; uniform int u_atest; uniform float u_aref; uniform bool u_fog_on, u_chroma_on;\n"
    "out vec4 o;\n"
    "vec3 fac3(int f, vec3 l, float la, float oa, vec4 t) {\n"
    "  if (f == 0) return vec3(0.0); if (f == 1) return l; if (f == 2) return vec3(oa); if (f == 3) return vec3(la);\n"
    "  if (f == 4) return vec3(t.a); if (f == 5) return t.rgb; if (f == 8) return vec3(1.0); if (f == 9) return 1.0 - l;\n"
    "  if (f == 10) return vec3(1.0 - oa); if (f == 11) return vec3(1.0 - la); if (f == 12) return vec3(1.0 - t.a); return vec3(1.0);\n"
    "}\n"
    "float fac1(int f, float la, float oa, vec4 t) {\n"
    "  if (f == 0) return 0.0; if (f == 1 || f == 3) return la; if (f == 2) return oa; if (f == 4) return t.a; if (f == 5) return 0.0;\n"
    "  if (f == 8) return 1.0; if (f == 9 || f == 11) return 1.0 - la; if (f == 10) return 1.0 - oa; if (f == 12) return 1.0 - t.a; return 1.0;\n"
    "}\n"
    "vec3 comb3(int fn, vec3 f, vec3 l, vec3 ot, float la) {\n"
    "  if (fn == 0) return vec3(0.0); if (fn == 1) return l; if (fn == 2) return vec3(la); if (fn == 3) return f * ot;\n"
    "  if (fn == 4) return f * ot + l; if (fn == 5) return f * ot + la; if (fn == 6) return f * (ot - l);\n"
    "  if (fn == 7) return f * (ot - l) + l; if (fn == 8) return f * (ot - l) + la; if (fn == 9) return l - f * l; return la - f * l;\n"
    "}\n"
    "float comb1(int fn, float f, float l, float ot) {\n"
    "  if (fn == 0) return 0.0; if (fn == 1 || fn == 2) return l; if (fn == 3) return f * ot;\n"
    "  if (fn == 4 || fn == 5) return f * ot + l; if (fn == 6) return f * (ot - l);\n"
    "  if (fn == 7 || fn == 8) return f * (ot - l) + l; return l - f * l;\n"
    "}\n"
    "void main() {\n"
    "  vec4 it = clamp(v_colw / v_w, 0.0, 1.0);\n"
    "  vec4 tx = vec4(1.0);\n"
    "  if (u_has_tex) {\n"
    "    vec4 texel = texture(u_tex, v_st);\n"     /* TMU0: local = texel, other = 0 */
    "    vec3 trgb = comb3(u_tc.x, fac3(u_tc.y, texel.rgb, texel.a, 0.0, texel), texel.rgb, vec3(0.0), texel.a);\n"
    "    float ta = comb1(u_tc.z, fac1(u_tc.w, texel.a, 0.0, texel), texel.a, 0.0);\n"
    "    if (u_inv.z != 0) trgb = 1.0 - trgb; if (u_inv.w != 0) ta = 1.0 - ta;\n"
    "    tx = clamp(vec4(trgb, ta), 0.0, 1.0);\n"
    "  }\n"
    /* local/other selections: 0 iterated, 1 constant (local) / texture (other), 2 depth (local) / constant (other) */
    "  vec3 cl = u_cc.z == 0 ? it.rgb : (u_cc.z == 1 ? u_const.rgb : vec3(gl_FragCoord.z));\n"
    "  vec3 co = u_cc.w == 0 ? it.rgb : (u_cc.w == 1 ? tx.rgb : u_const.rgb);\n"
    "  float al = u_ac.z == 0 ? it.a : (u_ac.z == 1 ? u_const.a : gl_FragCoord.z);\n"
    "  float ao = u_ac.w == 0 ? it.a : (u_ac.w == 1 ? tx.a : u_const.a);\n"
    "  vec3 rgb = comb3(u_cc.x, fac3(u_cc.y, cl, al, ao, tx), cl, co, al);\n"
    "  float a = comb1(u_ac.x, fac1(u_ac.y, al, ao, tx), al, ao);\n"
    "  if (u_inv.x != 0) rgb = 1.0 - rgb; if (u_inv.y != 0) a = 1.0 - a;\n"
    "  rgb = clamp(rgb, 0.0, 1.0); a = clamp(a, 0.0, 1.0);\n"
    "  if (u_chroma_on && all(lessThan(abs(rgb - u_chroma.rgb), vec3(2.5 / 255.0)))) discard;\n"
    "  float a8 = floor(a * 255.0 + 0.5);\n"
    "  bool pass = u_atest == 7 || (u_atest == 1 && a8 < u_aref) || (u_atest == 2 && a8 == u_aref) || (u_atest == 3 && a8 <= u_aref)\n"
    "           || (u_atest == 4 && a8 > u_aref) || (u_atest == 5 && a8 != u_aref) || (u_atest == 6 && a8 >= u_aref);\n"
    "  if (!pass) discard;\n"
    "  if (u_fog_on) {\n"
    "    float w = 1.0 / gl_FragCoord.w;\n"
    "    float f = texture(u_fog, vec2((log2(max(w, 1.0)) * 16.0 + 0.5) / 256.0, 0.5)).r;\n"
    "    rgb = mix(rgb, u_fogc.rgb, f);\n"
    "  }\n"
    "  o = vec4(rgb, a);\n"
    "}\n";
/* textured quad for overlays / software frames / the final blit */
static const char *VSB =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_p; uniform vec4 u_rect; out vec2 v_uv;\n"
    "void main() { v_uv = a_p; gl_Position = vec4(u_rect.xy + a_p * u_rect.zw, 0.0, 1.0); }\n";
static const char *FSB =
    "#version 300 es\n"
    "precision mediump float; in vec2 v_uv; uniform sampler2D u_t; uniform bool u_swz; uniform bool u_key; out vec4 o;\n"
    "void main() { vec4 c = texture(u_t, v_uv); if (u_key && c.a < 0.5) discard; o = vec4(u_swz ? c.bgr : c.rgb, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); rt_fatal("shader: %s", log); }
    return s;
}
static GLuint link(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs)); glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); rt_fatal("link: %s", log); }
    return p;
}

static int gl_ready;
static void gl_init(void) {
    prog = link(VS, FS);
    u_res = glGetUniformLocation(prog, "u_res"); u_has_tex = glGetUniformLocation(prog, "u_has_tex");
    u_cc = glGetUniformLocation(prog, "u_cc"); u_ac = glGetUniformLocation(prog, "u_ac"); u_tc = glGetUniformLocation(prog, "u_tc");
    u_inv = glGetUniformLocation(prog, "u_inv"); u_const = glGetUniformLocation(prog, "u_const"); u_fogc = glGetUniformLocation(prog, "u_fogc");
    u_chroma = glGetUniformLocation(prog, "u_chroma"); u_atest = glGetUniformLocation(prog, "u_atest"); u_aref = glGetUniformLocation(prog, "u_aref");
    u_fog_on = glGetUniformLocation(prog, "u_fog_on"); u_chroma_on = glGetUniformLocation(prog, "u_chroma_on");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0); glUniform1i(glGetUniformLocation(prog, "u_fog"), 1);
    prog_blit = link(VSB, FSB);
    ub_swz = glGetUniformLocation(prog_blit, "u_swz"); ub_rect = glGetUniformLocation(prog_blit, "u_rect"); ub_key = glGetUniformLocation(prog_blit, "u_key");
    glUseProgram(prog_blit); glUniform1i(glGetUniformLocation(prog_blit, "u_t"), 0);

    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GVert), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GVert), (void *)16);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GVert), (void *)32);
    glGenVertexArrays(1, &quad_vao); glBindVertexArray(quad_vao);
    glGenBuffers(1, &quad_vbo); glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    static const float q[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
    glBindVertexArray(0);

    GLuint t[3]; glGenTextures(3, t); fog_tex = t[0]; ovl_tex = t[1]; dd_tex = t[2];
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, t[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, i == 0 ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, i == 0 ? GL_LINEAR : (i == 2 ? GL_LINEAR : GL_NEAREST));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl_ready = 1;
    rt_log("gpu: %s / %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
}

static void ensure_fbo(int w, int h) {
    if (fbo && fbo_w == w && fbo_h == h) return;
    if (!fbo) { glGenFramebuffers(1, &fbo); glGenTextures(1, &fbo_color); glGenRenderbuffers(1, &fbo_depth); }
    glBindTexture(GL_TEXTURE_2D, fbo_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindRenderbuffer(GL_RENDERBUFFER, fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) rt_log("gpu: framebuffer incomplete");
    fbo_w = w; fbo_h = h;
}

static void upload_tex(int slot) {
    GTex *t = &texs[slot];
    if (!t->rgba) return;
    if (!gltex[slot]) glGenTextures(1, &gltex[slot]);
    glBindTexture(GL_TEXTURE_2D, gltex[slot]);
    if (gltex_ver[slot] == t->version) return;
    const uint32_t *p = t->rgba; int w = t->w, h = t->h;
    for (int l = 0; l < t->levels; l++) {
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
        p += w * h; if (w > 1) w >>= 1; if (h > 1) h >>= 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
    gltex_ver[slot] = t->version; gltex_par[slot] = 0;
}

static const GLenum cmp_gl[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
static GLenum blend_gl(int f, int src) {
    switch (f) {
    case 0: return GL_ZERO; case 1: return GL_SRC_ALPHA; case 2: return src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 3: return GL_ONE;  /* no destination alpha on Voodoo2 */
    case 4: return GL_ONE; case 5: return GL_ONE_MINUS_SRC_ALPHA; case 6: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 7: return GL_ZERO; case 15: return src ? GL_SRC_ALPHA_SATURATE : GL_ZERO;
    default: return src ? GL_ONE : GL_ZERO;
    }
}
static void color4(GLint loc, uint32_t argb) {
    glUniform4f(loc, (float)((argb >> 16) & 255) / 255.f, (float)((argb >> 8) & 255) / 255.f, (float)(argb & 255) / 255.f, (float)(argb >> 24) / 255.f);
}

static int scale;
static void scissor(const int16_t *clip, int fh) {
    int x0 = clip[0], y0 = clip[1], x1 = clip[2], y1 = clip[3];
    if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
    glScissor(x0 * scale, (fh - y1) * scale, (x1 - x0) * scale, (y1 - y0) * scale);
}

static void apply_state(const GState *s, const GFrame *f) {
    int has_tex = s->tex >= 0 && s->tex < GPU_MAX_TEX && texs[s->tex].rgba;
    if (has_tex) {
        upload_tex(s->tex);
        int mip = s->tex_mip && texs[s->tex].levels > 1;
        int cs = s->tex_clamp_s < 3 ? s->tex_clamp_s : 0, ct = s->tex_clamp_t < 3 ? s->tex_clamp_t : 0;
        uint32_t par = 1 + (uint32_t)(s->tex_min_linear != 0) + 2u * mip + 4u * (s->tex_mag_linear != 0) + 8u * cs + 32u * ct;
        if (gltex_par[s->tex] != par) {    /* every GL call costs on the web: set only what changed */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->tex_min_linear ? (mip ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR) : (mip ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->tex_mag_linear ? GL_LINEAR : GL_NEAREST);
            static const GLenum wrap[3] = { GL_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT };
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[cs]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[ct]);
            gltex_par[s->tex] = par;
        }
    }
    glUniform1i(u_has_tex, has_tex);
    glUniform4i(u_cc, s->cc_func, s->cc_factor, s->cc_local, s->cc_other);
    glUniform4i(u_ac, s->ac_func, s->ac_factor, s->ac_local, s->ac_other);
    glUniform4i(u_tc, s->tc_rgb_func, s->tc_rgb_factor, s->tc_a_func, s->tc_a_factor);
    glUniform4i(u_inv, s->cc_invert, s->ac_invert, s->tc_rgb_invert, s->tc_a_invert);
    color4(u_const, s->constant_color); color4(u_fogc, s->fog_color); color4(u_chroma, s->chroma_color);
    glUniform1i(u_atest, s->alpha_test); glUniform1f(u_aref, (float)s->alpha_ref);
    glUniform1i(u_fog_on, (s->fog_mode & 0xFF) != 0); glUniform1i(u_chroma_on, s->chroma_mode != 0);
    if (s->blend_src == 4 && s->blend_dst == 0) glDisable(GL_BLEND);
    else { glEnable(GL_BLEND); glBlendFuncSeparate(blend_gl(s->blend_src, 1), blend_gl(s->blend_dst, 0), blend_gl(s->blend_asrc, 1), blend_gl(s->blend_adst, 0)); }
    if (s->depth_mode) { glEnable(GL_DEPTH_TEST); glDepthFunc(cmp_gl[s->depth_func & 7]); glDepthMask(s->depth_mask ? GL_TRUE : GL_FALSE); }
    else { glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); }
    scissor(s->clip, f->h);
}

static void draw_quad_tex(GLuint t, float x0, float y0, float x1, float y1, int swz, int key) {
    /* rect in NDC, texture row 0 at the top */
    glUseProgram(prog_blit);
    glUniform4f(ub_rect, x0, y0, x1 - x0, y1 - y0);
    glUniform1i(ub_swz, swz); glUniform1i(ub_key, key);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t);
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(vao);
}

static void render_frame(GFrame *f) {
    ensure_fbo(f->w * scale, f->h * scale);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, f->w * scale, f->h * scale);
    glEnable(GL_SCISSOR_TEST);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(f->nverts * sizeof(GVert)), f->verts, GL_STREAM_DRAW);
    glUseProgram(prog);
    glUniform2f(u_res, (float)f->w, (float)f->h);
    static uint8_t fog_last[256]; static int fog_set;
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, fog_tex);
    if (!fog_set || memcmp(fog_last, f->fog_lut, sizeof fog_last)) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED, GL_UNSIGNED_BYTE, f->fog_lut);
        memcpy(fog_last, f->fog_lut, sizeof fog_last); fog_set = 1;
    }
    glActiveTexture(GL_TEXTURE0);
    uint32_t applied = UINT32_MAX;
    gpu_tex_lock();
    for (uint32_t i = 0; i < f->ncmds; i++) {
        GCmd *c = &f->cmds[i];
        if (c->type == GC_DRAW) {
            if (c->state != applied) { apply_state(&f->states[c->state], f); applied = c->state; }
            glDrawArrays(GL_TRIANGLES, (GLint)c->first, (GLsizei)c->count);
        } else if (c->type == GC_CLEAR) {
            scissor(c->clip, f->h);
            GLbitfield bits = 0;
            if (c->clear_color_on) {
                bits |= GL_COLOR_BUFFER_BIT;
                glClearColor((float)((c->color >> 16) & 255) / 255.f, (float)((c->color >> 8) & 255) / 255.f, (float)(c->color & 255) / 255.f, 1.f);
            }
            if (c->clear_depth_on) { bits |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); glClearDepthf(c->depth); }
            if (bits) glClear(bits);
            applied = UINT32_MAX;
        } else if (c->type == GC_LFB) {
            GLfb *l = &f->lfbs[c->lfb];
            glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
            glBindTexture(GL_TEXTURE_2D, ovl_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, l->w, l->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, l->rgba);
            draw_quad_tex(ovl_tex, -1, 1, 1, -1, 0, 1);
            glUseProgram(prog);
            glEnable(GL_SCISSOR_TEST);
            applied = UINT32_MAX;
        }
    }
    gpu_tex_unlock();
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
}

static void letterbox(int out_w, int out_h, int src_w, int src_h, int *x, int *y, int *w, int *h) {
    double a = (double)src_w / src_h;
    if ((double)out_w / out_h > a) { *h = out_h; *w = (int)(out_h * a + 0.5); } else { *w = out_w; *h = (int)(out_w / a + 0.5); }
    *x = (out_w - *w) / 2; *y = (out_h - *h) / 2;
}

int gpu_render(int out_w, int out_h) {
    if (!gl_ready) gl_init();
    renderer_seen = now_s();
    pthread_mutex_lock(&fq_mu);
    GFrame *nf = pending; pending = NULL;
    if (nf) pthread_cond_broadcast(&fq_cv);
    pthread_mutex_unlock(&fq_mu);
    int fresh = nf != NULL;
    if (nf) { gpu_frame_free(shown); shown = nf; }

    /* which source is current: the GPU frames or software (DirectDraw) frames? */
    int use_dd = last_dd_frame > last_gl_frame + 0.1 || !shown;
    int src_w = 640, src_h = 480;
    if (use_dd) {
        pthread_mutex_lock(&dd_mu);
        if (dd_px) {
            src_w = dd_w; src_h = dd_h;
            if (dd_new) {
                glBindTexture(GL_TEXTURE_2D, dd_tex);
                if (dd_bpp == 16) {
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, dd_pitch / 2);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, dd_w, dd_h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, dd_px);
                } else {
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, dd_pitch / 4);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dd_w, dd_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, dd_px);
                }
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                dd_new = 0; fresh = 1;
            }
        }
        pthread_mutex_unlock(&dd_mu);
    } else {
        src_w = shown->w; src_h = shown->h;
        int s = out_h / src_h; if (s > max_scale) s = max_scale; if (s < 1) s = 1;
        scale = s;
        if (fresh || !fbo) render_frame(shown);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, gpu_target_fbo);
    glViewport(0, 0, out_w, out_h);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    int x, y, w, h; letterbox(out_w, out_h, src_w, src_h, &x, &y, &w, &h);
    glViewport(x, y, w, h);
    if (use_dd) { if (dd_px) draw_quad_tex(dd_tex, -1, 1, 1, -1, dd_bpp == 32, 0); }
    else {
        /* the FBO is bottom-up: draw it flipped back */
        glUseProgram(prog_blit);
        glUniform4f(ub_rect, -1, -1, 2, 2); glUniform1i(ub_swz, 0); glUniform1i(ub_key, 0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, fbo_color);
        glBindVertexArray(quad_vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glBindVertexArray(vao);
    }
    return fresh;
}

#if defined(__EMSCRIPTEN__)
/* the page lowers this when the GPU can't keep up (main.js, perf log) */
EMSCRIPTEN_KEEPALIVE void rt_gpu_set_max_scale(int s) { max_scale = s < 1 ? 1 : s > 4 ? 4 : s; }
/* [published, replaced, wait ms, scale, fbo w, fbo h]: counters are totals since start */
EMSCRIPTEN_KEEPALIVE double *rt_gpu_stats(void) {
    static double out[6];
    out[0] = st_published; out[1] = st_replaced; out[2] = st_wait_ms; out[3] = scale; out[4] = fbo_w; out[5] = fbo_h;
    return out;
}
/* called by the page every animation frame (main browser thread) */
EMSCRIPTEN_KEEPALIVE int rt_gpu_frame(int w, int h) {
    static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx;
    if (!ctx) {
        EmscriptenWebGLContextAttributes at; emscripten_webgl_init_context_attributes(&at);
        at.majorVersion = 2; at.minorVersion = 0; at.alpha = 0; at.depth = 0; at.stencil = 0; at.antialias = 0;
        at.preserveDrawingBuffer = 0; at.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
        ctx = emscripten_webgl_create_context("#canvas", &at);
        if (ctx <= 0) { rt_log("WebGL2 context creation failed (%d)", (int)ctx); return -1; }
        emscripten_webgl_make_context_current(ctx);
    }
    return gpu_render(w, h);
}
#endif
#endif /* renderer */

#if !defined(__EMSCRIPTEN__)
void __attribute__((weak)) gpu_headless_frame(GFrame *f) { (void)f; }
#endif
