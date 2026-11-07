/* DirectSound subset for EA's eacsnd.dll.
 *
 * Buffers live in guest memory. Play cursors advance with wall-clock time at
 * the buffer's byte rate, which is what eacsnd's streaming mixer polls.
 * Output: snd_output_mix() is the hook a real backend (Web Audio worklet in
 * the browser, nothing natively yet) uses to pull the primary/secondary mix.
 */
#define _GNU_SOURCE
#include "rt.h"
#include <pthread.h>
#include <time.h>
#include <math.h>

#define DS_OK 0
#define DSERR_GENERIC 0x80004005u

typedef struct { const char *name; int nargs; hostfn fn; } Method;

typedef struct {
    uint32_t obj, mem, size;
    uint32_t rate, bytes_per_sec, block;
    uint16_t channels, bits;
    int primary, playing, looping;
    uint64_t start_ns;     /* time position 0 was at */
    uint32_t pos_at_stop;
    int32_t volume, pan; uint32_t freq;
    double mpos;           /* mixer-driven position in sample frames (when audio is on) */
} DSBuf;
static int g_audio_on;     /* a real output device pulls the mix */

#define MAX_BUF 256
static DSBuf bufs[MAX_BUF];
static int nbufs;
static pthread_mutex_t ds_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static DSBuf *buf_of(uint32_t obj) { uint32_t i = RD32(obj + 4); return i < (uint32_t)nbufs ? &bufs[i] : NULL; }

/* Native builds report the position the sound card has mixed up to. In the browser the mixer
 * thread can be starved for seconds (Safari), and the intro movie and music streaming poll this
 * position in a tight loop, so there play positions run on wall time like a real sound card and
 * the mixer follows them (snd_output_mix resyncs), never the other way round. */
#ifdef __EMSCRIPTEN__
#define POS_FROM_MIXER 0
#else
#define POS_FROM_MIXER g_audio_on
#endif
static uint32_t play_pos(DSBuf *b) {
    if (!b->playing || !b->size) return b->pos_at_stop;
    if (POS_FROM_MIXER && !b->primary) return ((uint32_t)b->mpos * b->block) % b->size;
    uint64_t bytes = (now_ns() - b->start_ns) * b->bytes_per_sec / 1000000000ull;
    if (!b->looping && bytes >= b->size) { b->playing = 0; b->pos_at_stop = 0; return 0; }
    return (uint32_t)(bytes % b->size) / b->block * b->block;
}

static uint32_t com_new(const char *iface, const Method *m, int n, uint32_t idx) {
    uint32_t vt = galloc(4 * (uint32_t)n), obj = galloc(16);
    for (int i = 0; i < n; i++) {
        char nm[96]; snprintf(nm, sizeof nm, "%s::%s", iface, m[i].name);
        WR32(vt + 4 * i, thunk_new(nm, m[i].fn, m[i].nargs));
    }
    WR32(obj, vt); WR32(obj + 4, idx); WR32(obj + 8, 1);
    return obj;
}
static uint32_t m_unimpl(CPU *c, const uint32_t *a) {
    rt_log("%s: unimplemented (this=%#x a1=%#x a2=%#x)", tls_thunk_name, a[0], a[1], a[2]);
    return DSERR_GENERIC;
}
static uint32_t m_ok(CPU *c, const uint32_t *a) { return DS_OK; }
static uint32_t m_addref(CPU *c, const uint32_t *a) { WR32(A(0) + 8, RD32(A(0) + 8) + 1); return RD32(A(0) + 8); }
static uint32_t m_release(CPU *c, const uint32_t *a) { uint32_t n = RD32(A(0) + 8); if (n) n--; WR32(A(0) + 8, n); return n; }

static void set_format(DSBuf *b, uint32_t wfx) {
    b->channels = RD16(wfx + 2); b->rate = RD32(wfx + 4); b->bits = RD16(wfx + 14);
    b->block = (uint32_t)b->channels * b->bits / 8; if (!b->block) b->block = 1;
    b->bytes_per_sec = b->rate * b->block; b->freq = b->rate;
}

/* ------------------------------------------------------- IDirectSoundBuffer */
static uint32_t b_GetCaps(CPU *c, const uint32_t *a) {
    DSBuf *b = buf_of(A(0)); WR32(A(1) + 4, b->primary ? 1 : 0); WR32(A(1) + 8, b->size); return DS_OK;
}
static uint32_t b_GetCurrentPosition(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&ds_mu);
    DSBuf *b = buf_of(A(0)); uint32_t p = play_pos(b);
    pthread_mutex_unlock(&ds_mu);
    if (A(1)) WR32(A(1), p);
    if (A(2)) WR32(A(2), b->size ? (p + b->bytes_per_sec / 100) % b->size : 0);  /* write cursor ~10 ms ahead */
    return DS_OK;
}
static uint32_t b_GetFormat(CPU *c, const uint32_t *a) {
    DSBuf *b = buf_of(A(0));
    if (A(1)) {
        WR16(A(1), 1); WR16(A(1) + 2, b->channels); WR32(A(1) + 4, b->rate); WR32(A(1) + 8, b->bytes_per_sec);
        WR16(A(1) + 12, (uint16_t)b->block); WR16(A(1) + 14, b->bits); if (A(2) >= 18) WR16(A(1) + 16, 0);
    }
    if (A(3)) WR32(A(3), 18);
    return DS_OK;
}
static uint32_t b_GetStatus(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&ds_mu);
    DSBuf *b = buf_of(A(0)); play_pos(b);
    uint32_t st = b->playing ? (1u | (b->looping ? 4u : 0u)) : 0u;
    pthread_mutex_unlock(&ds_mu);
    WR32(A(1), st);
    return DS_OK;
}
static uint32_t b_Lock(CPU *c, const uint32_t *a) {
    DSBuf *b = buf_of(A(0));
    uint32_t off = A(1), n = A(2), flags = A(7);
    if (flags & 2) n = b->size;                                   /* DSBLOCK_ENTIREBUFFER */
    if (flags & 1) { uint32_t w; pthread_mutex_lock(&ds_mu); w = play_pos(b); pthread_mutex_unlock(&ds_mu); off = w; } /* FROMWRITECURSOR */
    if (!b->size) return DSERR_GENERIC;
    off %= b->size; if (n > b->size) n = b->size;
    uint32_t n1 = n, n2 = 0;
    if (off + n > b->size) { n1 = b->size - off; n2 = n - n1; }
    WR32(A(3), b->mem + off); WR32(A(4), n1);
    if (A(5)) WR32(A(5), n2 ? b->mem : 0);
    if (A(6)) WR32(A(6), n2);
    return DS_OK;
}
static uint32_t b_Play(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&ds_mu);
    DSBuf *b = buf_of(A(0));
    if (!b->playing) {
        b->mpos = (double)(b->pos_at_stop / (b->block ? b->block : 1));
        b->playing = 1;
        b->start_ns = now_ns() - (uint64_t)b->pos_at_stop * 1000000000ull / (b->bytes_per_sec ? b->bytes_per_sec : 1);
    }
    b->looping = (A(3) & 1) != 0;                                  /* DSBPLAY_LOOPING */
    pthread_mutex_unlock(&ds_mu);
    return DS_OK;
}
static uint32_t b_Stop(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&ds_mu);
    DSBuf *b = buf_of(A(0)); b->pos_at_stop = play_pos(b); b->playing = 0;
    pthread_mutex_unlock(&ds_mu);
    return DS_OK;
}
static uint32_t b_SetCurrentPosition(CPU *c, const uint32_t *a) {
    pthread_mutex_lock(&ds_mu);
    DSBuf *b = buf_of(A(0)); b->pos_at_stop = b->size ? A(1) % b->size : 0;
    b->mpos = (double)(b->pos_at_stop / (b->block ? b->block : 1));
    if (b->playing) b->start_ns = now_ns() - (uint64_t)b->pos_at_stop * 1000000000ull / (b->bytes_per_sec ? b->bytes_per_sec : 1);
    pthread_mutex_unlock(&ds_mu);
    return DS_OK;
}
static uint32_t b_SetFormat(CPU *c, const uint32_t *a) {
    DSBuf *b = buf_of(A(0)); set_format(b, A(1));
    rt_log("DSound primary format: %u Hz, %u ch, %u bit", b->rate, b->channels, b->bits);
    return DS_OK;
}
static uint32_t b_SetVolume(CPU *c, const uint32_t *a) { buf_of(A(0))->volume = (int32_t)A(1); return DS_OK; }
static uint32_t b_SetPan(CPU *c, const uint32_t *a) { buf_of(A(0))->pan = (int32_t)A(1); return DS_OK; }
static uint32_t b_SetFrequency(CPU *c, const uint32_t *a) { buf_of(A(0))->freq = A(1); return DS_OK; }
static uint32_t b_GetVolume(CPU *c, const uint32_t *a) { WR32(A(1), (uint32_t)buf_of(A(0))->volume); return DS_OK; }
static uint32_t b_GetPan(CPU *c, const uint32_t *a) { WR32(A(1), (uint32_t)buf_of(A(0))->pan); return DS_OK; }
static uint32_t b_GetFrequency(CPU *c, const uint32_t *a) { WR32(A(1), buf_of(A(0))->freq); return DS_OK; }

static const Method buf_methods[] = {
    {"QueryInterface", 3, m_unimpl}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"GetCaps", 2, b_GetCaps}, {"GetCurrentPosition", 3, b_GetCurrentPosition}, {"GetFormat", 4, b_GetFormat},
    {"GetVolume", 2, b_GetVolume}, {"GetPan", 2, b_GetPan}, {"GetFrequency", 2, b_GetFrequency},
    {"GetStatus", 2, b_GetStatus}, {"Initialize", 3, m_ok}, {"Lock", 8, b_Lock}, {"Play", 4, b_Play},
    {"SetCurrentPosition", 2, b_SetCurrentPosition}, {"SetFormat", 2, b_SetFormat}, {"SetVolume", 2, b_SetVolume},
    {"SetPan", 2, b_SetPan}, {"SetFrequency", 2, b_SetFrequency}, {"Stop", 1, b_Stop},
    {"Unlock", 5, m_ok}, {"Restore", 1, m_ok},
};


/* ----------------------------------------------------------------- mixer */
#define OUT_RATE 44100
static double db_gain(int32_t hundredths) {      /* DirectSound volume: 0 .. -10000 (1/100 dB) */
    if (hundredths <= -10000) return 0.0;
    return pow(10.0, hundredths / 2000.0);
}
static inline double sample_at(DSBuf *b, uint32_t frame, int ch) {
    uint32_t off = (frame * b->block) % b->size;
    if (b->channels == 2) off += (uint32_t)ch * (b->bits / 8);
    if (b->bits == 16) return (int16_t)RD16(b->mem + off) / 32768.0;
    return ((int)RD8(b->mem + off) - 128) / 128.0;
}
/* Mix `frames` stereo frames into out (interleaved s16). Called by the audio backend. */
void snd_output_mix(int16_t *out, int frames) {
    static float acc[2 * 4096];
    if (frames > 4096) frames = 4096;
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    pthread_mutex_lock(&ds_mu);
    for (int i = 0; i < nbufs; i++) {
        DSBuf *b = &bufs[i];
        if (b->primary || !b->playing || !b->size || !b->block) continue;
        uint32_t nframes = b->size / b->block;
        double step = (double)(b->freq ? b->freq : b->rate) / OUT_RATE;
        double g = db_gain(b->volume);
        double gl = g * (b->pan > 0 ? db_gain(-b->pan) : 1.0), gr = g * (b->pan < 0 ? db_gain(b->pan) : 1.0);
        double p = b->mpos;
#ifdef __EMSCRIPTEN__
        /* keep fixed-rate buffers (movie sound, music streams) on the clock the game sees (it
         * fills them relative to that position); jump only when more than 60 ms apart */
        if (b->freq == b->rate || !b->freq) {
            uint64_t bytes = (now_ns() - b->start_ns) * b->bytes_per_sec / 1000000000ull;
            if (b->looping || bytes < b->size) {
                double target = (double)(bytes / b->block);
                double d = p - target;
                if (b->looping) { d = fmod(d, (double)nframes); if (d > nframes / 2.0) d -= nframes; if (d < -nframes / 2.0) d += nframes; }
                if (fabs(d) > 0.06 * b->rate) p = b->looping ? fmod(target, (double)nframes) : target < nframes ? target : nframes - 1;
            }
        }
#endif
        for (int k = 0; k < frames; k++) {
            uint32_t f0 = (uint32_t)p; double t = p - f0; uint32_t f1 = f0 + 1;
            if (f1 >= nframes) f1 = b->looping ? 0 : f0;
            double l0 = sample_at(b, f0, 0), l1 = sample_at(b, f1, 0);
            double l = l0 + (l1 - l0) * t, r = l;
            if (b->channels == 2) { double r0 = sample_at(b, f0, 1), r1 = sample_at(b, f1, 1); r = r0 + (r1 - r0) * t; }
            acc[2 * k] += (float)(l * gl); acc[2 * k + 1] += (float)(r * gr);
            p += step;
            if (p >= nframes) {
                if (b->looping) p -= nframes;
                else { b->playing = 0; b->pos_at_stop = 0; p = 0; break; }
            }
        }
        b->mpos = p;
    }
    pthread_mutex_unlock(&ds_mu);
    float peak = 0;
    for (int k = 0; k < 2 * frames; k++) {
        float v = acc[k] * 32767.0f;
        if (fabsf(acc[k]) > peak) peak = fabsf(acc[k]);
        out[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    static int stats = -1; static uint64_t done; static float pk; static int playing_max;
    if (stats < 0) stats = getenv("RT_AUDIO_STATS") != NULL;
    if (stats) {
        int np = 0; for (int i = 0; i < nbufs; i++) np += bufs[i].playing && !bufs[i].primary;
        if (np > playing_max) playing_max = np;
        if (peak > pk) pk = peak;
        done += (uint64_t)frames;
        if (done >= OUT_RATE) { rt_log("audio: %d buffers playing (max), peak %.3f", playing_max, pk); done = 0; pk = 0; playing_max = 0; }
    }
}

#ifdef __EMSCRIPTEN__
/* Browser: a producer thread keeps a ring of interleaved s16 stereo filled
 * ~90 ms ahead; the page's AudioWorklet (web/audio-worklet.js) drains it. */
#include <emscripten.h>
#include <unistd.h>
#define WEB_AUDIO_CAP 16384u                     /* frames, power of two */
typedef struct { volatile uint32_t rd, wr; uint32_t cap, rate; int16_t data[2 * WEB_AUDIO_CAP]; } WebAudio;
static WebAudio web_audio = {0, 0, WEB_AUDIO_CAP, OUT_RATE};
EMSCRIPTEN_KEEPALIVE void *rt_web_audio(void) { return &web_audio; }
/* The game's play positions run on wall time (play_pos), so nothing here can hold the game up.
 * This thread keeps the ring ~90 ms ahead of the speakers. If the page isn't draining it
 * (AudioContext suspended by the autoplay policy, worklet not loaded) it still mixes at wall
 * rate and drops the oldest samples, and after a late wake-up it mixes what it missed in one go,
 * so the mixer stays in step with the game. The page reports late wake-ups and drain stalls. */
#define WEB_AHEAD 4096u
static void *web_audio_main(void *unused) {
    (void)unused;
    int16_t tmp[2 * 512];
    uint64_t t0 = now_ns(), mixed = 0, late_max = 0, last = t0, stat_t = t0; unsigned drops = 0, lates = 0;
    for (;;) {
        uint64_t t = now_ns();
        if (t - last > late_max) late_max = t - last;
        if (t - last > 50000000ull) lates++;
        last = t;
        uint64_t due = (t - t0) * OUT_RATE / 1000000000ull;           /* frames the wall clock asks for */
        if (due > mixed + OUT_RATE) mixed = due - OUT_RATE;            /* more than 1 s behind: let it go */
        for (int burst = 0; burst < 128; burst++) {
            uint32_t rd = __atomic_load_n(&web_audio.rd, __ATOMIC_ACQUIRE), wr = web_audio.wr;
            int need = wr - rd < WEB_AHEAD || mixed + 512 <= due;      /* speakers or wall clock */
            if (!need) break;
            while (wr + 512 - rd > 2 * WEB_AHEAD) {                     /* not drained: drop the oldest */
                if (__atomic_compare_exchange_n(&web_audio.rd, &rd, wr + 512 - 2 * WEB_AHEAD, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { drops++; break; }
            }
            snd_output_mix(tmp, 512);
            for (int k = 0; k < 512; k++) {
                uint32_t i = (wr + (uint32_t)k) & (WEB_AUDIO_CAP - 1);
                web_audio.data[2 * i] = tmp[2 * k]; web_audio.data[2 * i + 1] = tmp[2 * k + 1];
            }
            __atomic_store_n(&web_audio.wr, wr + 512, __ATOMIC_RELEASE);
            mixed += 512;
        }
        if (t - stat_t > 10000000000ull) {                             /* every 10 s, only if something was off */
            if (lates || drops) rt_log("audio: thread woke late %u times (worst %.0f ms), %u drops of 512 frames (output not draining)", lates, late_max / 1e6, drops);
            lates = drops = 0; late_max = 0; stat_t = t;
        }
        usleep(2000);
    }
    return NULL;
}
static void audio_open(void) {
    if (g_audio_on) return;
    pthread_mutex_lock(&ds_mu);
    for (int i = 0; i < nbufs; i++) if (bufs[i].block) bufs[i].mpos = (double)(play_pos(&bufs[i]) / bufs[i].block);
    g_audio_on = 1;
    pthread_mutex_unlock(&ds_mu);
    pthread_t t; pthread_create(&t, NULL, web_audio_main, NULL);
    rt_log("audio: web ring %u Hz", OUT_RATE);
}
#elif !defined(RT_NO_SDL)
#include <SDL2/SDL.h>
static void sdl_audio_cb(void *ud, Uint8 *stream, int len) { (void)ud; snd_output_mix((int16_t *)stream, len / 4); }
static void audio_open(void) {
    if (g_audio_on || getenv("RT_HEADLESS") || getenv("RT_NOSOUND")) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { rt_log("audio: SDL_INIT_AUDIO failed: %s", SDL_GetError()); return; }
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512; want.callback = sdl_audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { rt_log("audio: open failed: %s", SDL_GetError()); return; }
    /* positions switch from wall clock to the mixer: carry them over */
    pthread_mutex_lock(&ds_mu);
    for (int i = 0; i < nbufs; i++) if (bufs[i].block) bufs[i].mpos = (double)(play_pos(&bufs[i]) / bufs[i].block);
    g_audio_on = 1;
    pthread_mutex_unlock(&ds_mu);
    SDL_PauseAudioDevice(dev, 0);
    rt_log("audio: %d Hz stereo via %s", have.freq, SDL_GetCurrentAudioDriver());
}
#else
static void audio_open(void) {}
#endif

/* ------------------------------------------------------------ IDirectSound */
static uint32_t ds_CreateSoundBuffer(CPU *c, const uint32_t *a) {
    uint32_t d = A(1), flags = RD32(d + 4), size = RD32(d + 8), wfx = RD32(d + 16);
    if (nbufs == MAX_BUF) return DSERR_GENERIC;
    DSBuf *b = &bufs[nbufs];
    memset(b, 0, sizeof *b);
    b->primary = (flags & 1) != 0;                                  /* DSBCAPS_PRIMARYBUFFER */
    if (b->primary) { b->channels = 2; b->rate = 22050; b->bits = 16; b->block = 4; b->bytes_per_sec = 88200; size = 88200 / 4; }
    else if (wfx) set_format(b, wfx);
    b->size = size; b->mem = size ? galloc(size) : 0;
    b->obj = com_new("DSBuffer", buf_methods, (int)(sizeof buf_methods / sizeof *buf_methods), (uint32_t)nbufs);
    nbufs++;
    if (rt_verbose > 1 || b->primary) rt_log("CreateSoundBuffer %s %u bytes, %u Hz %u ch %u bit -> %#x",
        b->primary ? "primary" : "secondary", size, b->rate, b->channels, b->bits, b->obj);
    WR32(A(2), b->obj);
    return DS_OK;
}
static uint32_t ds_GetCaps(CPU *c, const uint32_t *a) {
    uint32_t p = A(1), n = RD32(p);
    memset(G + p + 4, 0, n > 4 ? n - 4 : 92);
    WR32(p + 4, 0xF1F);           /* primary/secondary mono+stereo, 8+16 bit, continuous rate */
    WR32(p + 8, 4000); WR32(p + 12, 48000); WR32(p + 16, 1);
    return DS_OK;
}
static uint32_t ds_SetCooperativeLevel(CPU *c, const uint32_t *a) { rt_log("DSound SetCooperativeLevel(%u)", A(2)); return DS_OK; }
static uint32_t ds_GetSpeakerConfig(CPU *c, const uint32_t *a) { WR32(A(1), 4); return DS_OK; }  /* stereo */

static const Method ds_methods[] = {
    {"QueryInterface", 3, m_unimpl}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"CreateSoundBuffer", 4, ds_CreateSoundBuffer}, {"GetCaps", 2, ds_GetCaps}, {"DuplicateSoundBuffer", 3, m_unimpl},
    {"SetCooperativeLevel", 3, ds_SetCooperativeLevel}, {"Compact", 1, m_ok}, {"GetSpeakerConfig", 2, ds_GetSpeakerConfig},
    {"SetSpeakerConfig", 2, m_ok}, {"Initialize", 2, m_ok},
};

static uint32_t x_DirectSoundCreate(CPU *c, const uint32_t *a) {
    audio_open();
    uint32_t o = com_new("IDirectSound", ds_methods, (int)(sizeof ds_methods / sizeof *ds_methods), 0);
    rt_log("DirectSoundCreate -> %#x", o);
    WR32(A(1), o);
    return DS_OK;
}
const ImportDef imp_dsound[] = {
    {"DSOUND.DLL", "DirectSoundCreate", 3, x_DirectSoundCreate},
    {0},
};
