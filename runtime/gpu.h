/* GPU path: the Glide layer (glide.c, game thread) records each frame as draw
 * batches; the renderer (gpu.c, display thread natively / the page's main
 * thread in the browser) replays the latest complete frame with GL ES 3 /
 * WebGL2. The game thread never touches GL.
 *
 * Textures live in a shared table (converted to RGBA8 on the game thread when
 * the driver selects them); the renderer uploads entries whose version changed.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* one vertex as the renderer wants it */
typedef struct {
    float x, y;      /* window coords (game resolution), origin top-left */
    float depth;     /* 0..1, larger = farther */
    float w;         /* clip w (1/oow), 1 when the vertex has no Q */
    float r, g, b, a;/* 0..1 */
    float s, t;      /* texture coords, normalised 0..1 (perspective-correct via w) */
} GVert;

/* everything a draw call depends on */
typedef struct {
    int32_t tex;                         /* texture table slot, -1 = none */
    uint8_t tex_min_linear, tex_mag_linear, tex_clamp_s, tex_clamp_t, tex_mip;
    uint8_t cc_func, cc_factor, cc_local, cc_other, cc_invert;   /* grColorCombine */
    uint8_t ac_func, ac_factor, ac_local, ac_other, ac_invert;   /* grAlphaCombine */
    uint8_t tc_rgb_func, tc_rgb_factor, tc_a_func, tc_a_factor, tc_rgb_invert, tc_a_invert; /* grTexCombine (TMU0) */
    uint8_t alpha_test, alpha_ref;       /* GR_CMP_*, 0..255 */
    uint8_t blend_src, blend_dst, blend_asrc, blend_adst;      /* GR_BLEND_* */
    uint8_t depth_mode, depth_func, depth_mask;
    uint8_t fog_mode, chroma_mode;
    uint8_t color_mask;
    int16_t depth_bias;
    uint32_t constant_color;             /* ARGB */
    uint32_t fog_color;                  /* ARGB */
    uint32_t chroma_color;               /* ARGB */
    int16_t clip[4];                     /* minx, miny, maxx, maxy */
} GState;

enum { GC_DRAW = 1, GC_CLEAR = 2, GC_LFB = 3 };
typedef struct {
    uint8_t type;
    uint8_t clear_color_on, clear_depth_on;
    uint32_t state;                      /* GC_DRAW: index into states */
    uint32_t first, count;               /* GC_DRAW: vertices (triangles) */
    uint32_t color; float depth;         /* GC_CLEAR: ARGB, 0..1 */
    int16_t clip[4];                     /* GC_CLEAR */
    uint32_t lfb;                        /* GC_LFB: index into lfbs */
} GCmd;

typedef struct {                         /* a linear-framebuffer write the game made */
    int w, h;
    uint32_t *rgba;                      /* w*h, alpha 0 where the game wrote nothing */
} GLfb;

typedef struct GFrame {
    int w, h;                            /* game resolution */
    GCmd *cmds;   uint32_t ncmds, capcmds;
    GVert *verts; uint32_t nverts, capverts;
    GState *states; uint32_t nstates, capstates;
    GLfb *lfbs; uint32_t nlfbs, caplfbs;
    uint8_t fog_lut[256];                /* fog density by log2(w)*16, from grFogTable */
    uint32_t seq;
} GFrame;

/* texture table */
#define GPU_MAX_TEX 4096
typedef struct {
    int w, h, levels;                    /* level 0 = w x h, each next halves (min 1) */
    uint32_t *rgba;                      /* all levels back to back */
    uint32_t version;                    /* bumped on every re-conversion */
} GTex;

/* glide.c -> gpu.c */
void gpu_publish(GFrame *f);             /* takes ownership */
void gpu_tex_lock(void);
void gpu_tex_unlock(void);
GTex *gpu_tex(int slot);                 /* under the lock */
int gpu_tex_count(void);
extern int g_gpu_active;                 /* the Glide path has produced frames */

/* the renderer: draw the newest frame into the current framebuffer (out_w x out_h) */
int gpu_render(int out_w, int out_h);    /* returns 1 if something new was drawn */
void gpu_ddraw_frame(const uint8_t *px, int w, int h, int pitch, int bpp);  /* software frames (movies, softtri) */
GFrame *gpu_frame_new(void);
void gpu_frame_free(GFrame *f);
