/* KERNEL32 reimplementation. */
#define _GNU_SOURCE
#include "rt.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern uint32_t g_image_base, g_image_size;
extern char g_exe_path[1024];

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_NO_MORE_FILES 18
#define INVALID_HANDLE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102u
#define INFINITE 0xFFFFFFFFu

/* ------------------------------------------------------------ paths */
static char cwd_guest[260] = "C:\\NFS4";

/* Case-insensitive name lookup with a cache of directory listings. In the browser
 * every readdir of an OPFS folder costs tens of ms (NFS4 probes ~180 missing
 * showcase files while building its menus: seconds of stalls). Listings are
 * re-read after anything that changes the file system (fs_changed). */
typedef struct DirC { struct DirC *next; char *dir; char **names; int n, valid; } DirC;
static DirC *dir_cache[256];
static pthread_mutex_t dir_mu = PTHREAD_MUTEX_INITIALIZER;
static int dir_cache_off(void) { static int off = -1; if (off < 0) off = getenv("RT_NO_DIRCACHE") != NULL; return off; }
static DirC **dir_slot(const char *dir, size_t len) {
    unsigned h = 5381; for (size_t i = 0; i < len; i++) h = h * 33 + (unsigned char)dir[i];
    DirC **pp = &dir_cache[h & 255];
    while (*pp && (strlen((*pp)->dir) != len || memcmp((*pp)->dir, dir, len))) pp = &(*pp)->next;
    return pp;
}
/* a host path was created, removed or renamed: re-read its folder (and itself, if a folder) */
void fs_changed(const char *hp) {
    const char *sl = strrchr(hp, '/');
    pthread_mutex_lock(&dir_mu);
    DirC *c = *dir_slot(hp, strlen(hp)); if (c) c->valid = 0;
    if (sl && (c = *dir_slot(hp, (size_t)(sl - hp)))) c->valid = 0;
    pthread_mutex_unlock(&dir_mu);
}

static int ci_find(const char *dir, const char *name, char *out, size_t n) {
    if (dir_cache_off()) {
        DIR *d = opendir(dir);
        if (!d) return 0;
        struct dirent *e; int ok = 0;
        while ((e = readdir(d)))
            if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s", e->d_name); ok = 1; break; }
        closedir(d);
        return ok;
    }
    pthread_mutex_lock(&dir_mu);
    DirC **pp = dir_slot(dir, strlen(dir)), *c = *pp;
    if (!c) { c = *pp = calloc(1, sizeof *c); c->dir = strdup(dir); }
    if (!c->valid) {
        for (int i = 0; i < c->n; i++) free(c->names[i]);
        free(c->names); c->names = NULL; c->n = 0;
        DIR *d = opendir(dir);
        if (d) {
            int cap = 0; struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                if (c->n == cap) { cap = cap ? cap * 2 : 32; c->names = realloc(c->names, cap * sizeof *c->names); }
                c->names[c->n++] = strdup(e->d_name);
            }
            closedir(d);
        }
        if (rt_verbose > 1) rt_log("dircache: listed %s (%d entries)", dir, c->n);
        c->valid = 1;
    }
    int ok = 0;
    for (int i = 0; i < c->n; i++)
        if (!strcasecmp(c->names[i], name)) { snprintf(out, n, "%s", c->names[i]); ok = 1; break; }
    pthread_mutex_unlock(&dir_mu);
    return ok;
}

int fs_map(const char *gp, char *out, size_t outsz) {
    char tmp[1024], rel[1024];
    snprintf(tmp, sizeof tmp, "%s", gp);
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    const char *s = tmp;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        s += 2;
        /* absolute guest paths are rooted at the game dir's parent "C:\NFS4" */
        if (!strncasecmp(s, "/NFS4", 5) && (s[5] == '/' || !s[5])) s += 5;
    }
    while (*s == '/') s++;
    while (!strncmp(s, "./", 2)) s += 2;
    snprintf(rel, sizeof rel, "%s", s);
    char cur[1024]; snprintf(cur, sizeof cur, "%s", g_game_dir);
    char *save = NULL, *tok = strtok_r(rel, "/", &save);
    int missing = 0;
    while (tok) {
        char *next = strtok_r(NULL, "/", &save);
        char real[256];
        if (!strcmp(tok, ".")) { tok = next; continue; }
        if (!strcmp(tok, "..")) {
            size_t l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/..");
        } else if (!missing && ci_find(cur, tok, real, sizeof real)) {
            size_t l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/%s", real);
        } else {
            missing = 1;
            size_t l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/%s", tok);
        }
        tok = next;
    }
    snprintf(out, outsz, "%s", cur);
    return !missing;   /* 1: every component exists (case-insensitively) */
}

/* ------------------------------------------------------------ files */
static uint32_t set_event(uint32_t h, int sig, int pulse);
typedef struct { int fd; char path[1024]; } File;

static int is_nul_device(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    return (b[0] | 32) == 'n' && (b[1] | 32) == 'u' && (b[2] | 32) == 'l' && (!b[3] || b[3] == '.' || b[3] == ':');
}
static uint32_t k_CreateFileA(CPU *c, const uint32_t *a) {
    const char *gp = HSTR(A(0)); uint32_t access = A(1), disp = A(4);
    if (!gp || !*gp) { rt_log("CreateFileA(\"\") rejected (empty path)"); tls_lasterr = 3; return INVALID_HANDLE; }
    char hp[1024]; int found = fs_map(gp, hp, sizeof hp);
    if (is_nul_device(gp)) {   /* Windows' NUL device (any dir, any extension, "nul:"): reads EOF, writes vanish */
        snprintf(hp, sizeof hp, "/dev/null");
        disp = 3;   /* OPEN_EXISTING */
    }
    struct stat dst;
    if (!stat(hp, &dst) && S_ISDIR(dst.st_mode)) { tls_lasterr = 5; return INVALID_HANDLE; }  /* ERROR_ACCESS_DENIED */
    int fl = 0;
    if ((access & 0xC0000000u) == 0xC0000000u) fl = O_RDWR;
    else if (access & 0x40000000u) fl = O_WRONLY;
    else fl = O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 4: fl |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: fl |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    }
    if (!found && !(fl & O_CREAT) && !is_nul_device(gp)) {   /* known missing: no host round trip */
        if (rt_verbose) rt_log("CreateFileA(%s) -> not found (%s)", gp, hp);
        tls_lasterr = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE;
    }
    int fd = open(hp, fl, 0644);
    if (fd >= 0 && !found && (fl & O_CREAT)) fs_changed(hp);
    if (fd < 0 && is_nul_device(gp)) fd = open("/tmp/rt-nul", O_RDWR | O_CREAT | O_TRUNC, 0644);  /* no /dev/null */
    if (fd < 0) {
        if (rt_verbose) rt_log("CreateFileA(%s) -> not found (%s)", gp, hp);
        tls_lasterr = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE;
    }
    File *f = calloc(1, sizeof *f); f->fd = fd; snprintf(f->path, sizeof f->path, "%s", hp);
    uint32_t h = handle_new(H_FILE, f);
    if (rt_verbose > 1) rt_log("CreateFileA(%s) -> %#x", gp, h);
    return h;
}
static File *getfile(uint32_t h) {
    if (h == 0xFFFFFFF5u || h == 0xFFFFFFF4u || h == 0xFFFFFFF6u) { /* std handles */
        static File out = {1, "stdout"}, err = {2, "stderr"}, in = {0, "stdin"};
        return h == 0xFFFFFFF5u ? &out : h == 0xFFFFFFF4u ? &err : &in;
    }
    return handle_get(h, H_FILE);
}
static uint32_t k_ReadFile(CPU *c, const uint32_t *a) {
    File *f = getfile(A(0)); if (!f) return 0;
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    ssize_t n;
    uint32_t ov = A(4);
    if (ov) {   /* OVERLAPPED: Internal@0 InternalHigh@4 Offset@8 OffsetHigh@12 hEvent@16; completed synchronously */
        pos = (off_t)(((uint64_t)RD32(ov + 12) << 32) | RD32(ov + 8));
        n = pread(f->fd, G + A(1), A(2), pos);
        WR32(ov, 0); WR32(ov + 4, n < 0 ? 0 : (uint32_t)n);
        if (RD32(ov + 16)) set_event(RD32(ov + 16), 1, 0);
    } else n = read(f->fd, G + A(1), A(2));
    if (A(3)) WR32(A(3), n < 0 ? 0 : (uint32_t)n);
    { extern volatile uint64_t g_io_read_bytes, g_io_reads; if (n > 0) g_io_read_bytes += (uint64_t)n; g_io_reads++; }
    if (rt_verbose >= 3) rt_log("ReadFile(h=%#x %s @%ld, %u bytes -> %#x, ov=%#x) = %zd", A(0), f->path, (long)pos, A(2), A(1), A(4), n);
    return n >= 0;
}
static uint32_t k_WriteFile(CPU *c, const uint32_t *a) {
    File *f = getfile(A(0)); if (!f) return 0;
    ssize_t n = write(f->fd, G + A(1), A(2));
    if (A(3)) WR32(A(3), n < 0 ? 0 : (uint32_t)n);
    return n >= 0;
}
static uint32_t k_SetFilePointer(CPU *c, const uint32_t *a) {
    File *f = getfile(A(0)); if (!f) return INVALID_HANDLE;
    int64_t off = (int32_t)A(1);
    if (A(2)) off = (int64_t)(((uint64_t)RD32(A(2)) << 32) | A(1));
    off_t r = lseek(f->fd, off, A(3) == 0 ? SEEK_SET : A(3) == 1 ? SEEK_CUR : SEEK_END);
    if (rt_verbose >= 3) rt_log("SetFilePointer(h=%#x %s, %lld, %u) = %lld", A(0), f->path, (long long)off, A(3), (long long)r);
    if (A(2)) WR32(A(2), (uint32_t)((uint64_t)r >> 32));
    return (uint32_t)r;
}
static uint32_t k_GetFileSize(CPU *c, const uint32_t *a) {
    File *f = getfile(A(0));
    if (rt_verbose >= 3) rt_log("GetFileSize(h=%#x %s)", A(0), f ? f->path : "?");
    if (!f) return INVALID_HANDLE;
    struct stat st; fstat(f->fd, &st);
    if (A(1)) WR32(A(1), (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}
static uint32_t k_GetFileType(CPU *c, const uint32_t *a) {
    if (A(0) >= 0xFFFFFFF4u) return 2; /* FILE_TYPE_CHAR */
    return getfile(A(0)) ? 1 : 0;
}
static uint32_t k_SetEndOfFile(CPU *c, const uint32_t *a) {
    File *f = getfile(A(0)); if (!f) return 0;
    off_t p = lseek(f->fd, 0, SEEK_CUR); return ftruncate(f->fd, p) == 0;
}
static uint32_t k_GetOverlappedResult(CPU *c, const uint32_t *a) {
    if (A(2)) WR32(A(2), RD32(A(1) + 4));
    return 1;
}
static uint32_t k_FlushFileBuffers(CPU *c, const uint32_t *a) { (void)c; (void)a; return 1; }
static uint32_t k_GetFileAttributesA(CPU *c, const uint32_t *a) {
    char hp[1024];
    struct stat st;
    if (!fs_map(HSTR(A(0)), hp, sizeof hp) || stat(hp, &st)) { tls_lasterr = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
static uint32_t k_SetFileAttributesA(CPU *c, const uint32_t *a) { (void)c; (void)a; return 1; }
static uint32_t k_DeleteFileA(CPU *c, const uint32_t *a) {
    char hp[1024]; fs_map(HSTR(A(0)), hp, sizeof hp); int r = unlink(hp) == 0; fs_changed(hp); return r;
}
static uint32_t k_MoveFileA(CPU *c, const uint32_t *a) {
    char h1[1024], h2[1024]; fs_map(HSTR(A(0)), h1, sizeof h1); fs_map(HSTR(A(1)), h2, sizeof h2);
    int r = rename(h1, h2) == 0; fs_changed(h1); fs_changed(h2); return r;
}
static uint32_t k_CreateDirectoryA(CPU *c, const uint32_t *a) {
    char hp[1024]; fs_map(HSTR(A(0)), hp, sizeof hp); int r = mkdir(hp, 0755) == 0; fs_changed(hp); return r;
}
static uint32_t k_RemoveDirectoryA(CPU *c, const uint32_t *a) {
    char hp[1024]; fs_map(HSTR(A(0)), hp, sizeof hp); int r = rmdir(hp) == 0; fs_changed(hp); return r;
}
static uint32_t k_GetCurrentDirectoryA(CPU *c, const uint32_t *a) {
    uint32_t n = (uint32_t)strlen(cwd_guest);
    if (A(0) <= n) return n + 1;            /* too small: required size incl. NUL */
    memcpy(G + A(1), cwd_guest, n + 1);
    return n;
}
static uint32_t k_SetCurrentDirectoryA(CPU *c, const uint32_t *a) {
    rt_log("SetCurrentDirectoryA(%s)", HSTR(A(0))); return 1;
}
static uint32_t k_GetFullPathNameA(CPU *c, const uint32_t *a) {
    const char *p = HSTR(A(0)); char buf[520];
    if (isalpha((unsigned char)p[0]) && p[1] == ':') snprintf(buf, sizeof buf, "%s", p);
    else snprintf(buf, sizeof buf, "%s\\%s", cwd_guest, p);
    uint32_t n = (uint32_t)strlen(buf);
    if (rt_verbose > 1) rt_log("GetFullPathNameA(\"%s\", len=%u, buf=%#x) -> \"%s\"", p, A(1), A(2), buf);
    if (!*p) guest_backtrace(c);
    if (A(1) <= n) return n + 1;            /* too small: required size incl. NUL */
    memcpy(G + A(2), buf, n + 1);
    if (A(3)) { char *s = strrchr(HSTR(A(2)), '\\'); WR32(A(3), s ? A(2) + (uint32_t)(s - HSTR(A(2))) + 1 : A(2)); }
    return n;
}
static uint32_t k_GetDiskFreeSpaceA(CPU *c, const uint32_t *a) {
    if (A(1)) WR32(A(1), 8); if (A(2)) WR32(A(2), 512);
    if (A(3)) WR32(A(3), 0x100000); if (A(4)) WR32(A(4), 0x200000);
    return 1;
}

/* FindFirstFile: WIN32_FIND_DATAA attr@0 sizeHigh@28 sizeLow@32 name@44 */
typedef struct { char dir[1024], pat[260]; DIR *d; } Find;
static int find_next(Find *fd, uint32_t out) {
    struct dirent *e;
    while ((e = readdir(fd->d))) {
        if (fnmatch(fd->pat, e->d_name, FNM_CASEFOLD)) continue;
        char full[1300]; snprintf(full, sizeof full, "%s/%s", fd->dir, e->d_name);
        struct stat st; if (stat(full, &st)) continue;
        memset(G + out, 0, 320);
        WR32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        WR32(out + 32, (uint32_t)st.st_size);
        snprintf(HSTR(out + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
static uint32_t k_FindFirstFileA(CPU *c, const uint32_t *a) {
    char hp[1024]; fs_map(HSTR(A(0)), hp, sizeof hp);
    Find *f = calloc(1, sizeof *f);
    char *sl = strrchr(hp, '/');
    *sl = 0; snprintf(f->dir, sizeof f->dir, "%s", hp); snprintf(f->pat, sizeof f->pat, "%s", sl + 1);
    if (!strcmp(f->pat, "*.*")) strcpy(f->pat, "*");
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, A(1))) {
        if (f->d) closedir(f->d);
        free(f); tls_lasterr = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE;
    }
    return handle_new(H_FIND, f);
}
static uint32_t k_FindNextFileA(CPU *c, const uint32_t *a) {
    Find *f = handle_get(A(0), H_FIND);
    if (!f || !find_next(f, A(1))) { tls_lasterr = ERROR_NO_MORE_FILES; return 0; }
    return 1;
}
static uint32_t k_FindClose(CPU *c, const uint32_t *a) {
    Find *f = handle_get(A(0), H_FIND);
    if (f) { closedir(f->d); free(f); handle_close(A(0)); }
    return 1;
}

/* File mappings: read whole file into guest memory. */
typedef struct { File *f; uint32_t size; } Mapping;
static uint32_t views[256];
static uint32_t k_CreateFileMappingA(CPU *c, const uint32_t *a) {
    File *f = getfile(A(0));
    Mapping *m = calloc(1, sizeof *m); m->f = f; m->size = A(4);
    if (f && !m->size) { struct stat st; fstat(f->fd, &st); m->size = (uint32_t)st.st_size; }
    return handle_new(H_MAPPING, m);
}
static uint32_t k_MapViewOfFile(CPU *c, const uint32_t *a) {
    Mapping *m = handle_get(A(0), H_MAPPING); if (!m) return 0;
    uint32_t off = A(3), n = A(4) ? A(4) : m->size - off;
    uint32_t p = galloc(n);
    if (m->f) pread(m->f->fd, G + p, n, off);
    for (int i = 0; i < 256; i++) if (!views[i]) { views[i] = p; break; }
    return p;
}
static uint32_t k_UnmapViewOfFile(CPU *c, const uint32_t *a) {
    for (int i = 0; i < 256; i++) if (views[i] && views[i] == A(0)) { views[i] = 0; gfree(A(0)); return 1; }
    return 0;
}

/* ------------------------------------------------------------- handles */
typedef struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    int manual, signaled;        /* event */
    int is_mutex; pthread_t owner; int count;
    int is_thread;
    uint32_t pulse_gen;          /* bumped by PulseEvent so pollers can't miss it */
} Waitable;
static int evlog = -1;   /* RT_EVLOG=1: log event traffic as it happens; 2: keep it in a ring, printed by the watchdog */
typedef struct { uint64_t t; const char *what; uint32_t h, from, x; pthread_t th; } EvRec;
static EvRec evring[1 << 14]; static uint32_t evn;
static void ev(const char *what, uint32_t h, uint32_t x) {
    if (evlog < 0) { const char *e = getenv("RT_EVLOG"); evlog = e ? atoi(e) : 0; }
    if (!evlog) return;
    if (evlog == 1) { rt_log("ev %s %#x (%u) from %#x", what, h, x, rt_caller()); return; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t i = __atomic_fetch_add(&evn, 1, __ATOMIC_RELAXED) & 0x3fff;
    evring[i] = (EvRec){ (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec, what, h, rt_caller(), x, pthread_self() };
}
void ev_dump(void) {
    if (evlog != 2) return;
    uint32_t n = evn, k = n > 0x4000 ? n - 0x4000 : 0;
    rt_log("last %u events:", n - k);
    for (; k < n; k++) { EvRec *r = &evring[k & 0x3fff]; rt_log("  %10.3f th=%lx %s %#x (%u) from %#x", r->t / 1e9, (unsigned long)r->th, r->what, r->h, r->x, r->from); }
}
/* WaitForMultipleObjects sleeps on this until any object changes (a signal wakes it at
 * once: a short timed poll would cost a whole 15.6 ms timer tick in a browser on Windows) */
static pthread_mutex_t any_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t any_cv = PTHREAD_COND_INITIALIZER;
static uint32_t any_gen;
static void any_changed(void) { pthread_mutex_lock(&any_mu); any_gen++; pthread_cond_broadcast(&any_cv); pthread_mutex_unlock(&any_mu); }
static Waitable *waitable_new(void) {
    Waitable *w = calloc(1, sizeof *w);
    pthread_mutex_init(&w->mu, NULL); pthread_cond_init(&w->cv, NULL);
    return w;
}
static Waitable *get_waitable(uint32_t h) {
    int t = handle_type(h);
    if (t == H_EVENT || t == H_MUTEX || t == H_THREAD) return handle_get(h, t);
    return NULL;
}
static uint32_t k_CloseHandle(CPU *c, const uint32_t *a) {
    int t = handle_type(A(0));
    if (t == H_EVENT || t == H_MUTEX) ev("close", A(0), (uint32_t)t);
    if (t == H_FILE) { File *f = handle_get(A(0), H_FILE); close(f->fd); free(f); }
    if (t != H_THREAD && t != H_NONE) handle_close(A(0));
    return 1;
}
static uint32_t k_DuplicateHandle(CPU *c, const uint32_t *a) {
    if (A(3)) WR32(A(3), A(1)); return 1;
}

static int wait_one_gen(Waitable *w, uint32_t ms, uint32_t gen0);
static int wait_one(Waitable *w, uint32_t ms) { return wait_one_gen(w, ms, w->pulse_gen); }
static int wait_one_gen(Waitable *w, uint32_t ms, uint32_t gen0) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000;
    if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    rt_tick_round(&dl);
    pthread_mutex_lock(&w->mu);
    int ok = 1;
    for (;;) {
        if (w->is_mutex) {
            if (!w->count || pthread_equal(w->owner, pthread_self())) { w->owner = pthread_self(); w->count++; break; }
        } else if (w->signaled) {
            if (!w->manual && !w->is_thread) w->signaled = 0;
            break;
        } else if (w->pulse_gen != gen0) {
            break;                                   /* released by PulseEvent */
        }
        if (ms == 0) { ok = 0; break; }
        int r = ms == INFINITE ? pthread_cond_wait(&w->cv, &w->mu) : pthread_cond_timedwait(&w->cv, &w->mu, &dl);
        if (r == ETIMEDOUT) { ok = 0; break; }
    }
    pthread_mutex_unlock(&w->mu);
    return ok;
}
static uint32_t k_WaitForSingleObject(CPU *c, const uint32_t *a) {
    Waitable *w = get_waitable(A(0));
    if (!w) { rt_log("WaitForSingleObject on non-waitable %#x", A(0)); return WAIT_OBJECT_0; }
    return wait_one(w, A(1)) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static uint32_t k_WaitForMultipleObjectsEx(CPU *c, const uint32_t *a) {
    uint32_t n = A(0), arr = A(1), all = A(2), ms = A(3);
    if (ms == INFINITE) ev(all ? "waitall" : "wait", RD32(arr), n);
    uint32_t gens[64];
    for (uint32_t i = 0; i < n && i < 64; i++) { Waitable *w = get_waitable(RD32(arr + 4 * i)); gens[i] = w ? w->pulse_gen : 0; }
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    if (ms != INFINITE) {
        dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
        rt_tick_round(&dl);
    }
    for (;;) {
        pthread_mutex_lock(&any_mu); uint32_t gen = any_gen; pthread_mutex_unlock(&any_mu);
        if (!all) {
            for (uint32_t i = 0; i < n; i++) {
                Waitable *w = get_waitable(RD32(arr + 4 * i));
                if (w && wait_one_gen(w, 0, gens[i])) { if (ms == INFINITE) ev("woke", RD32(arr + 4 * i), i); return WAIT_OBJECT_0 + i; }
            }
        } else {
            /* bWaitAll: only take (reset) the objects once every one of them is ready,
             * otherwise an auto-reset event could be consumed without the wait succeeding */
            uint32_t ready = 0;
            for (uint32_t i = 0; i < n; i++) {
                Waitable *w = get_waitable(RD32(arr + 4 * i));
                if (!w) { ready++; continue; }
                pthread_mutex_lock(&w->mu);
                int ok = w->is_mutex ? (!w->count || pthread_equal(w->owner, pthread_self())) : (w->signaled || w->pulse_gen != gens[i]);
                pthread_mutex_unlock(&w->mu);
                ready += ok;
            }
            if (ready == n) {
                for (uint32_t i = 0; i < n; i++) { Waitable *w = get_waitable(RD32(arr + 4 * i)); if (w) wait_one_gen(w, 0, gens[i]); }
                return WAIT_OBJECT_0;
            }
        }
        if (ms == 0) return WAIT_TIMEOUT;
        pthread_mutex_lock(&any_mu);
        int r = 0;
        while (any_gen == gen && r != ETIMEDOUT) r = ms == INFINITE ? pthread_cond_wait(&any_cv, &any_mu) : pthread_cond_timedwait(&any_cv, &any_mu, &dl);
        pthread_mutex_unlock(&any_mu);
        if (r == ETIMEDOUT) return WAIT_TIMEOUT;
    }
}
static uint32_t k_CreateEventA(CPU *c, const uint32_t *a) {
    Waitable *w = waitable_new(); w->manual = A(1) != 0; w->signaled = A(2) != 0;
    uint32_t h = handle_new(H_EVENT, w);
    ev(w->manual ? "create manual" : "create auto", h, (uint32_t)w->signaled);
    return h;
}
static uint32_t set_event(uint32_t h, int sig, int pulse) {
    ev(pulse ? "pulse" : sig ? "set" : "reset", h, 0);
    Waitable *w = handle_get(h, H_EVENT); if (!w) return 0;
    pthread_mutex_lock(&w->mu);
    w->signaled = sig;
    if (pulse) { w->signaled = 0; w->pulse_gen++; }
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    if (sig) any_changed();
    return 1;
}
static uint32_t k_SetEvent(CPU *c, const uint32_t *a) { return set_event(A(0), 1, 0); }
uint32_t event_signal(uint32_t h) { return set_event(h, 1, 0); }   /* for host-side producers */
static uint32_t k_ResetEvent(CPU *c, const uint32_t *a) { return set_event(A(0), 0, 0); }
static uint32_t k_PulseEvent(CPU *c, const uint32_t *a) { return set_event(A(0), 1, 1); }
static uint32_t k_CreateMutexA(CPU *c, const uint32_t *a) {
    Waitable *w = waitable_new(); w->is_mutex = 1;
    if (A(1)) { w->owner = pthread_self(); w->count = 1; }
    return handle_new(H_MUTEX, w);
}

/* critical sections: host mutexes keyed by guest address (open-addressing hash; slots
 * are never freed, so lookups need no lock once the address is in) */
#define MAX_CS 1024
static struct { volatile uint32_t addr; pthread_mutex_t mu; } cs_tab[MAX_CS];
static pthread_mutex_t cs_tab_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_get(uint32_t addr) {
    uint32_t h = (addr * 2654435761u) >> 22;            /* 10 bits */
    for (uint32_t k = 0; k < MAX_CS; k++) {
        uint32_t i = (h + k) & (MAX_CS - 1), v = __atomic_load_n(&cs_tab[i].addr, __ATOMIC_ACQUIRE);
        if (v == addr) return &cs_tab[i].mu;
        if (!v) break;
    }
    pthread_mutex_lock(&cs_tab_mu);
    for (uint32_t k = 0; k < MAX_CS; k++) {
        uint32_t i = (h + k) & (MAX_CS - 1);
        if (cs_tab[i].addr == addr) { pthread_mutex_unlock(&cs_tab_mu); return &cs_tab[i].mu; }
        if (!cs_tab[i].addr) {
            pthread_mutexattr_t at; pthread_mutexattr_init(&at);
            pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&cs_tab[i].mu, &at);
            __atomic_store_n(&cs_tab[i].addr, addr, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&cs_tab_mu);
            return &cs_tab[i].mu;
        }
    }
    rt_fatal("too many critical sections");
}
static uint32_t k_InitializeCriticalSection(CPU *c, const uint32_t *a) { cs_get(A(0)); return 0; }
static uint32_t k_DeleteCriticalSection(CPU *c, const uint32_t *a) { (void)a; return 0; }
/* single-core mode: an uncontended Enter must not hand the CPU to another thread
 * (NFS4's loader enters ~1000 times per menu screen); only a real wait drops the lock */
static uint32_t k_EnterCriticalSection(CPU *c, const uint32_t *a) {
    pthread_mutex_t *m = cs_get(A(0));
    if (pthread_mutex_trylock(m)) { int d = bgl_drop(); pthread_mutex_lock(m); bgl_restore(d); }
    return 0;
}
static uint32_t k_LeaveCriticalSection(CPU *c, const uint32_t *a) { pthread_mutex_unlock(cs_get(A(0))); return 0; }

/* ------------------------------------------------------------- threads */
typedef struct {
    Waitable w;   /* must be first: signaled when thread exits */
    uint32_t start, param, stack;
    pthread_t th; uint32_t tid;
    pthread_mutex_t smu; pthread_cond_t scv; int suspended;
    volatile int prio;
} Thread;
static uint32_t next_tid = 0x100;

static void *thread_main(void *p) {
    Thread *t = p;
    pthread_mutex_lock(&t->smu);
    while (t->suspended) pthread_cond_wait(&t->scv, &t->smu);
    pthread_mutex_unlock(&t->smu);
    CPU *c = cpu_new_thread(t->stack ? t->stack : 0x100000);
    tls_cpu = c;
    rt_watch_thread(t->tid);
    bgl_set_prio_ptr(&t->prio);
    ev("thread start", t->tid, t->param);
    uint32_t r = guest_call(c, t->start, 1, t->param);
    rt_log("thread %#x exited (%u)", t->tid, r);
    pthread_mutex_lock(&t->w.mu); t->w.signaled = 1; pthread_cond_broadcast(&t->w.cv); pthread_mutex_unlock(&t->w.mu);
    any_changed();
    return NULL;
}
static uint32_t k_CreateThread(CPU *c, const uint32_t *a) {
    Thread *t = calloc(1, sizeof *t);
    pthread_mutex_init(&t->w.mu, NULL); pthread_cond_init(&t->w.cv, NULL); t->w.is_thread = 1; t->w.manual = 1;
    pthread_mutex_init(&t->smu, NULL); pthread_cond_init(&t->scv, NULL);
    t->stack = A(1); t->start = A(2); t->param = A(3); t->suspended = (A(4) & 4) != 0;
    t->tid = __atomic_fetch_add(&next_tid, 4, __ATOMIC_SEQ_CST);
    if (A(5)) WR32(A(5), t->tid);
    uint32_t h = handle_new(H_THREAD, t);
    rt_log("CreateThread start=%#x param=%#x%s", t->start, t->param, t->suspended ? " (suspended)" : "");
    pthread_create(&t->th, NULL, thread_main, t);
    return h;
}
static uint32_t k_ResumeThread(CPU *c, const uint32_t *a) {
    Thread *t = handle_get(A(0), H_THREAD); if (!t) return INVALID_HANDLE;
    pthread_mutex_lock(&t->smu); int was = t->suspended; t->suspended = 0; pthread_cond_signal(&t->scv); pthread_mutex_unlock(&t->smu);
    return (uint32_t)was;
}
static uint32_t k_ExitThread(CPU *c, const uint32_t *a) { rt_log("ExitThread(%u)", A(0)); pthread_exit(NULL); }
static uint32_t k_TerminateThread(CPU *c, const uint32_t *a) { rt_log("TerminateThread ignored"); return 1; }
static uint32_t k_GetThreadPriority(CPU *c, const uint32_t *a) {
    Thread *t = handle_get(A(0), H_THREAD); return t ? (uint32_t)t->prio : 0;
}
static uint32_t k_SetThreadPriority(CPU *c, const uint32_t *a) {
    Thread *t = handle_get(A(0), H_THREAD); if (t) t->prio = (int)A(1);
    if (rt_verbose) rt_log("SetThreadPriority(thread %#x, %d)", t ? t->tid : A(0), (int)A(1));
    return 1;
}
static uint32_t k_GetCurrentThread(CPU *c, const uint32_t *a) { return 0xFFFFFFFEu; }
static uint32_t k_GetCurrentThreadId(CPU *c, const uint32_t *a) { return (uint32_t)(c->fs_base >> 12); }
static uint32_t k_GetCurrentProcess(CPU *c, const uint32_t *a) { return 0xFFFFFFFFu; }
static uint32_t k_GetCurrentProcessId(CPU *c, const uint32_t *a) { return 0x42; }
static uint32_t k_SleepEx(CPU *c, const uint32_t *a) { rt_usleep(A(0) ? A(0) * 1000 : 0); if (!A(0)) sched_yield(); return 0; }
static uint32_t k_ExitProcess(CPU *c, const uint32_t *a) {
    fprintf(stderr, "[rt] ExitProcess(%u)\n", A(0));
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module.onGameExit) Module.onGameExit($0); }, A(0));
#endif
    exit((int)A(0));
}

/* TLS */
static uint32_t tls_used[64];
static __thread uint32_t tls_vals[64];
static uint32_t k_TlsAlloc(CPU *c, const uint32_t *a) {
    for (int i = 0; i < 64; i++) if (!__atomic_exchange_n(&tls_used[i], 1, __ATOMIC_SEQ_CST)) return (uint32_t)i;
    return INVALID_HANDLE;
}
static uint32_t k_TlsFree(CPU *c, const uint32_t *a) { if (A(0) < 64) tls_used[A(0)] = 0; return 1; }
static uint32_t k_TlsGetValue(CPU *c, const uint32_t *a) { tls_lasterr = 0; return A(0) < 64 ? tls_vals[A(0)] : 0; }
static uint32_t k_TlsSetValue(CPU *c, const uint32_t *a) { if (A(0) < 64) tls_vals[A(0)] = A(1); return 1; }

/* ---------------------------------------------------------------- memory */
static uint32_t k_GetProcessHeap(CPU *c, const uint32_t *a) { return 0x00050000u; }
static uint32_t k_HeapAlloc(CPU *c, const uint32_t *a) { return galloc(A(2)); }
static uint32_t k_HeapFree(CPU *c, const uint32_t *a) { gfree(A(2)); return 1; }
static uint32_t k_HeapReAlloc(CPU *c, const uint32_t *a) { return grealloc(A(2), A(3)); }
static uint32_t k_HeapSize(CPU *c, const uint32_t *a) { return gsize(A(2)); }
static uint32_t k_VirtualAlloc(CPU *c, const uint32_t *a) {
    if (A(0)) return A(0);   /* commit of a previously reserved range: memory already backed */
    uint32_t p = galloc(A(1) + 0xFFF);
    return p ? (p + 0xFFF) & ~0xFFFu : 0;
}
static uint32_t k_VirtualProtect(CPU *c, const uint32_t *a) { if (A(3)) WR32(A(3), 4); return 1; }
static uint32_t k_VirtualFree(CPU *c, const uint32_t *a) { return 1; }
static uint32_t k_VirtualQuery(CPU *c, const uint32_t *a) {
    /* MEMORY_BASIC_INFORMATION: base, allocbase, allocprot, size, state, protect, type */
    uint32_t p = A(1), b = A(0) & ~0xFFFu;
    WR32(p, b); WR32(p + 4, b); WR32(p + 8, 4); WR32(p + 12, 0x10000);
    WR32(p + 16, 0x1000); WR32(p + 20, 4); WR32(p + 24, 0x20000);
    return 28;
}
static uint32_t k_GlobalMemoryStatus(CPU *c, const uint32_t *a) {
    uint32_t p = A(0); WR32(p, 32); WR32(p + 4, 10);
    WR32(p + 8, 256u << 20); WR32(p + 12, 200u << 20); WR32(p + 16, 512u << 20);
    WR32(p + 20, 400u << 20); WR32(p + 24, 2047u << 20); WR32(p + 28, 1800u << 20);
    return 0;
}

/* ---------------------------------------------------------------- modules */
static uint32_t k_GetModuleHandleA(CPU *c, const uint32_t *a) {
    if (!A(0)) return g_image_base;
    const char *n = HSTR(A(0));
    rt_log("GetModuleHandleA(%s)", n);
    if (!strcasecmp(n, "kernel32.dll") || !strcasecmp(n, "kernel32")) return 0x7FF00000u;
    return module_load(n) ? module_load(n) : 0;
}
static uint32_t k_GetModuleFileNameA(CPU *c, const uint32_t *a) {
    const char *p = "C:\\NFS4\\nfs4.exe"; uint32_t n = (uint32_t)strlen(p);
    if (A(2) > n) memcpy(G + A(1), p, n + 1);
    return n;
}
static uint32_t k_GetModuleFileNameW(CPU *c, const uint32_t *a) {
    const char *p = "C:\\NFS4\\nfs4.exe"; uint32_t n = (uint32_t)strlen(p);
    for (uint32_t i = 0; i <= n && i < A(2); i++) WR16(A(1) + 2 * i, (uint8_t)p[i]);
    return n;
}
static uint32_t k_LoadLibraryA(CPU *c, const uint32_t *a) {
    uint32_t h = module_load(HSTR(A(0)));
    rt_log("LoadLibraryA(%s) -> %#x", HSTR(A(0)), h);
    if (!h) tls_lasterr = 126;
    return h;
}
static uint32_t k_FreeLibrary(CPU *c, const uint32_t *a) { return 1; }
static uint32_t k_GetProcAddress(CPU *c, const uint32_t *a) {
    if (A(1) < 0x10000) { rt_log("GetProcAddress by ordinal %u", A(1)); return 0; }
    const char *n = HSTR(A(1));
    if (A(0) == 0x7FF00000u) return import_thunk("KERNEL32.DLL", n);
    uint32_t p = module_proc(A(0), n);
    if (rt_verbose > 1) rt_log("GetProcAddress(%#x, %s) -> %#x", A(0), n, p);
    return p;
}

/* resources: walk .rsrc of the main image */
static uint32_t rsrc_find(uint32_t dir, uint32_t key, int byname, const char *name) {
    uint32_t rs = g_image_base + RD32(g_image_base + RD32(g_image_base + 0x3C) + 24 + 112);
    uint16_t nn = RD16(dir + 12), ni = RD16(dir + 14);
    for (uint32_t i = 0; i < (uint32_t)nn + ni; i++) {
        uint32_t e = dir + 16 + 8 * i, id = RD32(e), off = RD32(e + 4);
        int match;
        if (id & 0x80000000u) {
            if (!byname) continue;
            uint32_t s = rs + (id & 0x7FFFFFFF); uint16_t len = RD16(s); char buf[128]; uint16_t k;
            for (k = 0; k < len && k < 127; k++) buf[k] = (char)RD16(s + 2 + 2 * k);
            buf[k] = 0; match = !strcasecmp(buf, name);
        } else match = !byname && (id == key || key == 0xFFFFFFFFu);
        if (match) return off;
    }
    return 0;
}
static uint32_t k_FindResourceA(CPU *c, const uint32_t *a) {
    uint32_t dd = RD32(g_image_base + 0x3C) + 24 + 112;
    uint32_t rs = g_image_base + RD32(g_image_base + dd);
    uint32_t nm = A(1), ty = A(2), o;
    o = ty < 0x10000 ? rsrc_find(rs, ty, 0, NULL) : rsrc_find(rs, 0, 1, HSTR(ty));
    if (!o) return 0;
    uint32_t d2 = rs + (o & 0x7FFFFFFF);
    o = nm < 0x10000 ? rsrc_find(d2, nm, 0, NULL) : rsrc_find(d2, 0, 1, HSTR(nm));
    if (!o) return 0;
    uint32_t d3 = rs + (o & 0x7FFFFFFF);
    o = rsrc_find(d3, 0xFFFFFFFFu, 0, NULL);
    return o ? rs + o : 0;   /* data entry: rva, size */
}
static uint32_t k_LoadResource(CPU *c, const uint32_t *a) { return A(1) ? g_image_base + RD32(A(1)) : 0; }
static uint32_t k_LockResource(CPU *c, const uint32_t *a) { return A(0); }
static uint32_t k_SizeofResource(CPU *c, const uint32_t *a) { return A(1) ? RD32(A(1) + 4) : 0; }

/* --------------------------------------------------------------- environ */
static uint32_t cmdline, cmdlinew, envblock;
static uint32_t k_GetCommandLineA(CPU *c, const uint32_t *a) { return cmdline; }
static uint32_t k_GetCommandLineW(CPU *c, const uint32_t *a) { return cmdlinew; }
static uint32_t k_GetEnvironmentStrings(CPU *c, const uint32_t *a) { return envblock; }
static uint32_t k_FreeEnvironmentStringsA(CPU *c, const uint32_t *a) { return 1; }
static uint32_t k_SetEnvironmentVariableA(CPU *c, const uint32_t *a) { return 1; }
static uint32_t k_GetVersion(CPU *c, const uint32_t *a) { return 0x0A280105u; } /* XP SP-whatever, 5.1.2600 */
static uint32_t k_GetACP(CPU *c, const uint32_t *a) { return 1252; }
static uint32_t k_GetOEMCP(CPU *c, const uint32_t *a) { return 437; }
static uint32_t k_GetCPInfo(CPU *c, const uint32_t *a) {
    memset(G + A(1), 0, 20); WR32(A(1), 1); WR8(A(1) + 4, '?'); return 1;
}
static uint32_t k_GetStdHandle(CPU *c, const uint32_t *a) { return A(0); }
static uint32_t k_SetStdHandle(CPU *c, const uint32_t *a) { return 1; }
static uint32_t k_GetLastError(CPU *c, const uint32_t *a) { return tls_lasterr; }
static uint32_t k_SetLastError(CPU *c, const uint32_t *a) { tls_lasterr = A(0); return 0; }
static uint32_t k_SetErrorMode(CPU *c, const uint32_t *a) { return 0; }
static uint32_t k_SetUnhandledExceptionFilter(CPU *c, const uint32_t *a) { return 0; }
static uint32_t k_UnhandledExceptionFilter(CPU *c, const uint32_t *a) { return 0; }
static uint32_t k_OutputDebugStringA(CPU *c, const uint32_t *a) { rt_log("debug: %s", HSTR(A(0))); return 0; }
static uint32_t k_DebugBreak(CPU *c, const uint32_t *a) { rt_log("DebugBreak"); return 0; }
static uint32_t k_RtlUnwind(CPU *c, const uint32_t *a) { rt_log("RtlUnwind (SEH) not supported"); return 0; }
static uint32_t k_GetTickCount(CPU *c, const uint32_t *a) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static uint32_t k_GetLocalTime(CPU *c, const uint32_t *a) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); uint32_t p = A(0);
    WR16(p, (uint16_t)(tm.tm_year + 1900)); WR16(p + 2, (uint16_t)(tm.tm_mon + 1)); WR16(p + 4, (uint16_t)tm.tm_wday);
    WR16(p + 6, (uint16_t)tm.tm_mday); WR16(p + 8, (uint16_t)tm.tm_hour); WR16(p + 10, (uint16_t)tm.tm_min);
    WR16(p + 12, (uint16_t)tm.tm_sec); WR16(p + 14, 0);
    return 0;
}
static uint32_t k_GetTimeZoneInformation(CPU *c, const uint32_t *a) { memset(G + A(0), 0, 172); return 0; }
static uint32_t k_GetSystemInfo(CPU *c, const uint32_t *a) {
    uint32_t p = A(0); memset(G + p, 0, 36);
    WR32(p + 4, 0x1000); WR32(p + 8, 0x10000); WR32(p + 12, 0x7FFEFFFF); WR32(p + 16, 1);
    WR32(p + 20, 1); WR32(p + 24, 586); WR32(p + 28, 0x10000); WR16(p + 32, 6);
    return 0;
}
static uint32_t k_GetWindowsDirectoryA(CPU *c, const uint32_t *a) {
    const char *w = "C:\\WINDOWS"; if (A(1) > 10) strcpy(HSTR(A(0)), w); return 10;
}
static uint32_t k_MultiByteToWideChar(CPU *c, const uint32_t *a) {
    const uint8_t *s = (const uint8_t *)HSTR(A(2)); int n = (int)A(3);
    if (n < 0) n = (int)strlen((const char *)s) + 1;
    if (!A(5)) return (uint32_t)n;
    int k; for (k = 0; k < n && k < (int)A(5); k++) WR16(A(4) + 2 * k, s[k]);
    return (uint32_t)k;
}
static uint32_t k_WideCharToMultiByte(CPU *c, const uint32_t *a) {
    int n = (int)A(3);
    if (n < 0) { n = 0; while (RD16(A(2) + 2 * n)) n++; n++; }
    if (!A(5)) return (uint32_t)n;
    int k; for (k = 0; k < n && k < (int)A(5); k++) { uint16_t ch = RD16(A(2) + 2 * k); WR8(A(4) + k, ch < 256 ? (uint8_t)ch : '?'); }
    return (uint32_t)k;
}
static uint32_t k_GetConsoleMode(CPU *c, const uint32_t *a) { return 0; }
static uint32_t k_WriteConsoleA(CPU *c, const uint32_t *a) {
    fwrite(G + A(1), 1, A(2), stderr); if (A(3)) WR32(A(3), A(2)); return 1;
}

/* ------------------------------------------------------------- INI files */
static int ini_lookup(const char *file, const char *sec, const char *key, char *out, size_t n) {
    char hp[1024];
    char gf[512]; snprintf(gf, sizeof gf, "%s", file);
    fs_map(gf, hp, sizeof hp);
    FILE *f = fopen(hp, "r"); if (!f) return 0;
    char line[1024]; int in = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (isspace((unsigned char)*s)) s++;
        char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
        if (*s == '[') { char *r = strchr(s, ']'); if (r) *r = 0; in = !strcasecmp(s + 1, sec); continue; }
        if (!in || !*s || *s == ';') continue;
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0; char *k2 = s, *ke = eq; while (ke > k2 && isspace((unsigned char)ke[-1])) *--ke = 0;
        char *v = eq + 1; while (isspace((unsigned char)*v)) v++;
        if (!strcasecmp(k2, key)) { snprintf(out, n, "%s", v); found = 1; break; }
    }
    fclose(f);
    return found;
}
static uint32_t k_GetPrivateProfileStringA(CPU *c, const uint32_t *a) {
    char v[1024];
    const char *sec = HSTR(A(0)), *key = HSTR(A(1)), *def = A(2) ? HSTR(A(2)) : "";
    if (!sec || !key) { rt_log("GetPrivateProfileStringA enumerate not supported"); if (A(4)) WR8(A(3), 0); return 0; }
    if (!ini_lookup(HSTR(A(5)), sec, key, v, sizeof v)) snprintf(v, sizeof v, "%s", def);
    uint32_t n = (uint32_t)strlen(v); if (n >= A(4)) n = A(4) ? A(4) - 1 : 0;
    if (A(4)) { memcpy(G + A(3), v, n); WR8(A(3) + n, 0); }
    rt_log("ini [%s] %s = \"%s\"", sec, key, v);
    return n;
}
static uint32_t k_GetPrivateProfileIntA(CPU *c, const uint32_t *a) {
    char v[64];
    uint32_t r = ini_lookup(HSTR(A(3)), HSTR(A(0)), HSTR(A(1)), v, sizeof v) ? (uint32_t)strtol(v, NULL, 0) : A(2);
    rt_log("ini [%s] %s = %d", HSTR(A(0)), HSTR(A(1)), (int)r);
    return r;
}
static uint32_t k_GetPrivateProfileSectionA(CPU *c, const uint32_t *a) {
    if (A(2) >= 2) { WR8(A(1), 0); WR8(A(1) + 1, 0); } return 0;
}

/* serial-port / console calls are only used by modem play: fail cleanly */
static uint32_t k_fail(CPU *c, const uint32_t *a) { return 0; }

void kernel32_init(void) {
    cmdline = gstrdup("C:\\NFS4\\nfs4.exe");
    cmdlinew = galloc(64);
    const char *s = "C:\\NFS4\\nfs4.exe";
    for (int i = 0; s[i]; i++) WR16(cmdlinew + 2 * i, (uint8_t)s[i]);
    envblock = galloc(16);
}

#define K(n, args) {"KERNEL32.DLL", #n, args, k_##n}
#define KS(n, args, fn) {"KERNEL32.DLL", #n, args, fn}
const ImportDef imp_kernel32[] = {
    K(CloseHandle, 1), K(GetProcessHeap, 0), K(CreateDirectoryA, 2), K(CreateEventA, 4),
    K(CreateFileA, 7), K(CreateFileMappingA, 6), K(CreateMutexA, 3), K(CreateThread, 6),
    K(DebugBreak, 0), K(DeleteCriticalSection, 1), K(DeleteFileA, 1), K(DuplicateHandle, 7),
    K(EnterCriticalSection, 1), KS(EscapeCommFunction, 2, k_fail), K(ExitProcess, 1), K(ExitThread, 1),
    K(FindClose, 1), K(FindFirstFileA, 2), K(FindNextFileA, 2), K(FlushFileBuffers, 1),
    K(FreeEnvironmentStringsA, 1), K(FreeLibrary, 1), K(GetACP, 0), K(GetCPInfo, 2),
    KS(GetCommModemStatus, 2, k_fail), KS(GetCommProperties, 2, k_fail), KS(GetCommState, 2, k_fail),
    K(GetCommandLineA, 0), K(GetCommandLineW, 0), K(GetConsoleMode, 2), K(GetCurrentDirectoryA, 2),
    K(GetCurrentProcess, 0), K(GetCurrentProcessId, 0), K(GetCurrentThreadId, 0), K(GetCurrentThread, 0),
    K(GetDiskFreeSpaceA, 5), K(HeapReAlloc, 4), K(GetEnvironmentStrings, 0), K(GetFileAttributesA, 1),
    K(GetFileSize, 2), K(HeapSize, 3), K(GetFileType, 1), K(GetFullPathNameA, 4), K(GetLastError, 0),
    K(GetLocalTime, 1), K(FindResourceA, 3), K(LoadResource, 2), K(GetModuleFileNameA, 3),
    K(GetModuleFileNameW, 3), K(GetModuleHandleA, 1), K(GetOEMCP, 0), K(GetOverlappedResult, 4),
    K(GetProcAddress, 2), K(GetStdHandle, 1), K(GetSystemInfo, 1), K(GetThreadPriority, 1),
    K(GetTickCount, 0), K(GetTimeZoneInformation, 1), K(LockResource, 1), K(GetVersion, 0),
    K(SizeofResource, 2), K(GetWindowsDirectoryA, 2), K(GlobalMemoryStatus, 1),
    K(InitializeCriticalSection, 1), K(LeaveCriticalSection, 1), K(LoadLibraryA, 1), K(MapViewOfFile, 5),
    K(MoveFileA, 2), K(MultiByteToWideChar, 6), K(OutputDebugStringA, 1), KS(PeekConsoleInputA, 4, k_fail),
    K(PulseEvent, 1), KS(PurgeComm, 2, k_fail), KS(ReadConsoleInputA, 4, k_fail), K(ReadFile, 5),
    K(RemoveDirectoryA, 1), K(ResetEvent, 1), K(ResumeThread, 1), K(RtlUnwind, 4),
    KS(SetCommState, 2, k_fail), KS(SetCommTimeouts, 2, k_fail), KS(SetConsoleCtrlHandler, 2, k_fail),
    KS(SetConsoleMode, 2, k_fail), K(SetCurrentDirectoryA, 1), K(SetEndOfFile, 1),
    K(SetEnvironmentVariableA, 2), KS(SetEnvironmentVariableW, 2, k_SetEnvironmentVariableA),
    K(SetErrorMode, 1), K(SetEvent, 1), K(SetFileAttributesA, 2), K(SetFilePointer, 4), K(SetLastError, 1),
    K(SetStdHandle, 2), K(SetThreadPriority, 2), K(SetUnhandledExceptionFilter, 1), K(SleepEx, 2),
    K(TerminateThread, 2), K(TlsAlloc, 0), K(TlsFree, 1), K(TlsGetValue, 1), K(TlsSetValue, 2),
    K(UnhandledExceptionFilter, 1), K(UnmapViewOfFile, 1), K(VirtualAlloc, 4), K(VirtualFree, 3),
    K(VirtualQuery, 3), K(WaitForMultipleObjectsEx, 5), K(WaitForSingleObject, 2),
    K(WideCharToMultiByte, 8), K(WriteConsoleA, 5), K(WriteFile, 5), K(HeapAlloc, 3), K(HeapFree, 3),
    K(GetPrivateProfileIntA, 4), K(GetPrivateProfileStringA, 6), K(GetPrivateProfileSectionA, 4),
    KS(Sleep, 1, k_SleepEx), KS(SetDllDirectoryA, 1, k_FlushFileBuffers), KS(VirtualProtect, 4, k_VirtualProtect),
    {0},
};
