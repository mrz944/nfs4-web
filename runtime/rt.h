/* Runtime internals shared by the Win32 reimplementation. */
#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ---- guest address space layout ----
 * Compact so the same layout fits a wasm32 heap allocation in the browser:
 *   0x00180000-0x00380000 main thread stack
 *   0x00400000            nfs4.exe            (lifted at its preferred base)
 *   0x00B00000            eacsnd.dll          (rebased by the lifter + loader)
 *   0x00C00000            voodoo2a.dll        (Glide3 thrash driver, rebased)
 *   0x01000000-GUEST_SIZE heap (HeapAlloc/VirtualAlloc/thread stacks/TEBs)
 * Thunks and the return sentinel are fake code addresses above GUEST_SIZE that
 * are only ever compared, never dereferenced. */
#define GUEST_SIZE      0x20000000u     /* 512 MiB; the game peaks around 100 MiB */
#define STACK_MAIN_TOP  0x00380000u
#define CODE_LO         0x00401000u     /* lifted code lives below HEAP_LO */
#define HEAP_LO         0x01000000u
#define HEAP_HI         GUEST_SIZE
#define THUNK_BASE      0xFE000000u     /* fake code addresses of host functions */
#define THUNK_STRIDE    16u
#define RET_SENTINEL    0xFDFFFFF0u     /* return address for host->guest calls */

/* ---- logging ---- */
extern int rt_verbose;
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- guest memory ---- */
uint32_t galloc(uint32_t size);          /* zeroed, 16-byte aligned */
void     gfree(uint32_t p);
uint32_t gsize(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gstrdup(const char *s);         /* host string -> new guest string */
static inline char *HSTR(uint32_t p) { return p ? (char *)(G + p) : NULL; }
static inline void *HPTR(uint32_t p) { return p ? (void *)(G + p) : NULL; }

/* ---- host functions callable from guest ---- */
typedef uint32_t (*hostfn)(CPU *c, const uint32_t *a);
/* nargs < 0 → cdecl (caller cleans); otherwise stdcall with nargs dwords */
uint32_t thunk_new(const char *name, hostfn fn, int nargs);
const char *thunk_name(uint32_t addr);

/* host -> guest call; restores esp afterwards so stdcall/cdecl both work */
uint32_t guest_call(CPU *c, uint32_t fn, int n, ...);
CPU *cpu_new_thread(uint32_t stack_size);
void guest_backtrace(CPU *c);  /* fresh CPU with stack + TEB */
extern __thread CPU *tls_cpu;
extern __thread const char *tls_thunk_name;  /* host function currently executing */
void rt_watch_thread(uint32_t tid);   /* register the calling guest thread with the watchdog */
void rt_watch_dump(const char *why);
uint32_t rt_caller(void);              /* guest return address of the API call in progress */   /* log what every guest thread is doing */

/* ---- import table ---- */
typedef struct { const char *dll, *name; int nargs; hostfn fn; } ImportDef;
const ImportDef *import_find(const char *dll, const char *name);
uint32_t import_thunk(const char *dll, const char *name);  /* for GetProcAddress */

/* ---- handles ---- */
enum { H_NONE, H_FILE, H_EVENT, H_MUTEX, H_THREAD, H_FIND, H_MAPPING, H_MODULE, H_WINDOW, H_TIMER, H_SOCKET, H_MISC };
typedef struct Handle Handle;
uint32_t handle_new(int type, void *obj);
void    *handle_get(uint32_t h, int type);
int      handle_type(uint32_t h);
void     handle_close(uint32_t h);

/* ---- filesystem ---- */
extern char g_game_dir[1024];
/* map a guest path ("C:\\x\\gamedata\\A.VIV", "fedata/x") to a host path;
 * case-insensitive per component. returns 0 if parent dir missing. */
int fs_map(const char *gpath, char *out, size_t outsz);

/* ---- per-DLL import tables ---- */
extern const ImportDef imp_kernel32[], imp_user32[], imp_gdi32[], imp_advapi32[],
    imp_ole32[], imp_shell32[], imp_winmm[], imp_wsock32[], imp_dinput[], imp_ddraw[], imp_dsound[],
    imp_extra[];  /* win32_extra.c: NFS4 additions */

/* ---- dynamically loaded modules (thrash driver, eacsnd) ---- */
uint32_t module_load(const char *name);         /* 0 if unknown */
uint32_t module_proc(uint32_t hmod, const char *name);

/* ---- windows/messages ---- */
void msg_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void user_init(void);

/* PE images */
uint32_t load_pe(const char *path, uint32_t *entry);
uint32_t load_pe_at(const char *path, uint32_t *entry, uint32_t base);  /* rebases if needed */
uint32_t pe_export(uint32_t base, const char *name);
uint32_t lifted_base(const char *dll);

uint32_t event_signal(uint32_t h);   /* SetEvent from host code */

/* last error */
extern __thread uint32_t tls_lasterr;

#define A(i) (a[i])

/* single-core mode (core.c): one guest thread runs at a time, like SingleProcAffinity=1 */
void bgl_acquire(void);
void bgl_release(void);
int  bgl_drop(void);          /* around a host-side wait: returns depth for bgl_restore */
void bgl_restore(int depth);
