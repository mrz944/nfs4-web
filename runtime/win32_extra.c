/* Imports NFS4 (nfs4.exe 4.50 modern patch) uses on top of NFS3's set.
 *
 * Mostly launcher / hardware-probe calls that only need to fail or answer
 * plausibly: child processes (the game can relaunch itself), version
 * resources, the mixer (CD audio volume), drives (CD detection, the modern
 * patch no longer needs the disc).
 */
#define _GNU_SOURCE
#include "rt.h"
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

static const ImportDef *k32(const char *name) {
    const ImportDef *d = import_find("KERNEL32.DLL", name);
    if (!d || !d->fn) rt_fatal("win32_extra: KERNEL32!%s missing", name);
    return d;
}

/* ---- KERNEL32 ---- */
static uint32_t x_CompareFileTime(CPU *c, const uint32_t *a) {
    uint64_t x = RD64(A(0)), y = RD64(A(1));
    return x < y ? 0xFFFFFFFFu : x > y ? 1u : 0u;
}
static uint32_t x_CreateProcessA(CPU *c, const uint32_t *a) {
    rt_log("CreateProcessA(%s, %s) refused", HSTR(A(0)) ? HSTR(A(0)) : "-", HSTR(A(1)) ? HSTR(A(1)) : "-");
    tls_lasterr = 2; return 0;
}
static uint32_t x_WinExec(CPU *c, const uint32_t *a) { rt_log("WinExec(%s) refused", HSTR(A(0))); return 2; }
static uint32_t x_GetExitCodeProcess(CPU *c, const uint32_t *a) { if (A(1)) WR32(A(1), 0); return 1; }
/* only C: exists, a fixed disk (the game folder) */
static uint32_t x_GetDriveTypeA(CPU *c, const uint32_t *a) {
    const char *p = HSTR(A(0));
    if (!p || (p[0] | 32) == 'c') return 3;   /* DRIVE_FIXED */
    return 1;                                 /* DRIVE_NO_ROOT_DIR */
}
static uint32_t x_GetLogicalDrives(CPU *c, const uint32_t *a) { (void)a; return 1u << 2; }
static uint32_t x_GetLogicalDriveStringsA(CPU *c, const uint32_t *a) {
    static const char s[] = "C:\\\0";
    if (A(0) < sizeof s) return sizeof s;
    memcpy(G + A(1), s, sizeof s);
    return sizeof s - 1;
}
static uint32_t x_GetFileTime(CPU *c, const uint32_t *a) {
    /* a fixed time: 2001-02-16 (the build date), as a FILETIME */
    const uint64_t ft = 126270000000000000ull;
    for (int i = 1; i <= 3; i++) if (A(i)) WR64(A(i), ft);
    return 1;
}
static uint32_t x_GetStartupInfoA(CPU *c, const uint32_t *a) {
    memset(G + A(0), 0, 68); WR32(A(0), 68); return 0;
}
static uint32_t x_GlobalAlloc(CPU *c, const uint32_t *a) { return galloc(A(1) ? A(1) : 1); }
static uint32_t x_GlobalFree(CPU *c, const uint32_t *a) { if (A(0)) gfree(A(0)); return 0; }
static uint32_t x_OpenMutexA(CPU *c, const uint32_t *a) { tls_lasterr = 2; return 0; }  /* no other instance running */
static uint32_t x_RaiseException(CPU *c, const uint32_t *a) {
    guest_backtrace(c);
    rt_fatal("RaiseException(%#x, flags %#x)", A(0), A(1));
}
static uint32_t x_SetProcessAffinityMask(CPU *c, const uint32_t *a) { return 1; }
/* _lopen(path, mode): OF_READ 0 / OF_WRITE 1 / OF_READWRITE 2 → CreateFileA */
static uint32_t x__lopen(CPU *c, const uint32_t *a) {
    uint32_t m = A(1) & 3;
    uint32_t acc = m == 0 ? 0x80000000u : m == 1 ? 0x40000000u : 0xC0000000u;
    uint32_t args[7] = {A(0), acc, 3, 0, 3 /* OPEN_EXISTING */, 0x80, 0};
    return k32("CreateFileA")->fn(c, args);
}
static uint32_t x__lclose(CPU *c, const uint32_t *a) {
    uint32_t args[1] = {A(0)};
    return k32("CloseHandle")->fn(c, args) ? 0 : 0xFFFFFFFFu;
}
/* WritePrivateProfileStringA(section, key, value, file): rewrite the ini in place */
static uint32_t x_WritePrivateProfileStringA(CPU *c, const uint32_t *a) {
    const char *sec = HSTR(A(0)), *key = HSTR(A(1)), *val = HSTR(A(2)), *file = HSTR(A(3));
    if (!sec || !file) return 0;
    char hp[1024]; fs_map(strchr(file, '\\') || strchr(file, '/') ? file : file, hp, sizeof hp);
    FILE *f = fopen(hp, "rb");
    char *src = NULL; size_t n = 0;
    if (f) { fseek(f, 0, SEEK_END); n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); src = malloc(n + 1); n = fread(src, 1, n, f); src[n] = 0; fclose(f); }
    else { src = calloc(1, 1); }
    size_t cap = n + strlen(sec) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + 64;
    char *out = malloc(cap); size_t o = 0;
    int in_sec = 0, done = 0, seen_sec = 0;
    for (char *line = src; line && *line;) {
        char *e = strchr(line, '\n'); size_t len = e ? (size_t)(e - line + 1) : strlen(line);
        char *t = line; while (*t == ' ' || *t == '\t') t++;
        if (*t == '[') {
            if (in_sec && !done && key && val) { o += (size_t)sprintf(out + o, "%s=%s\r\n", key, val); done = 1; }
            char *r = strchr(t, ']');
            in_sec = r && (size_t)(r - t - 1) == strlen(sec) && !strncasecmp(t + 1, sec, strlen(sec));
            if (in_sec) seen_sec = 1;
        } else if (in_sec && key) {
            size_t kl = strlen(key);
            if (!strncasecmp(t, key, kl) && (t[kl] == '=' || t[kl] == ' ')) {
                if (val && !done) { o += (size_t)sprintf(out + o, "%s=%s\r\n", key, val); done = 1; }
                line += len; continue;
            }
        }
        if (in_sec && !key) { line += len; continue; }     /* key NULL: delete section */
        if (!(in_sec && !key && *t == '[')) { memcpy(out + o, line, len); o += len; }
        line += len;
    }
    if (key && val && !done) {
        if (!seen_sec) o += (size_t)sprintf(out + o, "%s[%s]\r\n", o && out[o - 1] != '\n' ? "\r\n" : "", sec);
        else if (!in_sec) { /* section was earlier: append at end is close enough */ }
        o += (size_t)sprintf(out + o, "%s=%s\r\n", key, val);
    }
    f = fopen(hp, "wb");
    if (f) { fwrite(out, 1, o, f); fclose(f); fs_changed(hp); }
    free(src); free(out);
    rt_log("ini write [%s] %s = %s", sec, key ? key : "(section deleted)", val ? val : "(deleted)");
    return f != NULL;
}

/* ---- USER32 ---- */
static uint32_t x_LoadCursorA(CPU *c, const uint32_t *a) { return 0x7F00u; }
static uint32_t x_UnregisterClassA(CPU *c, const uint32_t *a) { return 1; }
static uint32_t x_DdeClientTransaction(CPU *c, const uint32_t *a) { return 0; }

/* ---- ADVAPI32 ---- */
static uint32_t x_RegSetValueExA(CPU *c, const uint32_t *a) { return 0; }

/* ---- VERSION: no version resources ---- */
static uint32_t x_GetFileVersionInfoSizeA(CPU *c, const uint32_t *a) { if (A(1)) WR32(A(1), 0); tls_lasterr = 1813; return 0; }
static uint32_t x_GetFileVersionInfoA(CPU *c, const uint32_t *a) { return 0; }
static uint32_t x_VerQueryValueA(CPU *c, const uint32_t *a) { return 0; }

/* ---- WINMM: no CD audio, no mixer ---- */
static uint32_t x_mciSendStringA(CPU *c, const uint32_t *a) {
    rt_log("mciSendStringA(%s)", HSTR(A(0)));
    if (A(1) && A(2)) WR8(A(1), 0);
    return 263;   /* MCIERR_INVALID_DEVICE_NAME */
}
static uint32_t x_mixerGetNumDevs(CPU *c, const uint32_t *a) { return 0; }
static uint32_t x_mixer_fail(CPU *c, const uint32_t *a) { return 2; }   /* MMSYSERR_BADDEVICEID */


/* ---- MSVC CRT start-up (the modern patch's voodoo2a.dll is an MSVC build) ---- */
static uint32_t x_HeapCreate(CPU *c, const uint32_t *a) { return 0x00050000u; }   /* = GetProcessHeap */
static uint32_t x_HeapDestroy(CPU *c, const uint32_t *a) { return 1; }
static uint32_t x_SetHandleCount(CPU *c, const uint32_t *a) { return A(0); }
static uint32_t x_InterlockedIncrement(CPU *c, const uint32_t *a) { return __atomic_add_fetch((uint32_t *)(G + A(0)), 1, __ATOMIC_SEQ_CST); }
static uint32_t x_InterlockedDecrement(CPU *c, const uint32_t *a) { return __atomic_sub_fetch((uint32_t *)(G + A(0)), 1, __ATOMIC_SEQ_CST); }
static uint32_t x_TerminateProcess(CPU *c, const uint32_t *a) { rt_log("TerminateProcess(%u)", A(1)); exit((int)A(1)); }
static uint32_t x_GetVersionExA(CPU *c, const uint32_t *a) {
    /* OSVERSIONINFOA: Windows XP 5.1.2600, VER_PLATFORM_WIN32_NT */
    uint32_t p = A(0), sz = RD32(p);
    memset(G + p + 4, 0, sz > 4 ? sz - 4 : 0);
    WR32(p + 4, 5); WR32(p + 8, 1); WR32(p + 12, 2600); WR32(p + 16, 2);
    return 1;
}
static uint32_t x_GetEnvironmentVariableA(CPU *c, const uint32_t *a) { tls_lasterr = 203; return 0; }
static uint32_t env_w;
static uint32_t x_GetEnvironmentStringsW(CPU *c, const uint32_t *a) { if (!env_w) env_w = galloc(16); return env_w; }
static uint32_t x_FreeEnvironmentStringsW(CPU *c, const uint32_t *a) { return 1; }
/* locale: the "C" locale is all the CRT needs; ANSI == Latin-1 */
static uint32_t x_GetStringTypeA(CPU *c, const uint32_t *a) {   /* (lcid, type, src, n, out) */
    int n = (int)A(3); const uint8_t *s = G + A(2);
    if (n < 0) n = (int)strlen((const char *)s) + 1;
    for (int i = 0; i < n; i++) {
        unsigned ch = s[i]; uint16_t t = 0;
        if (ch >= 'A' && ch <= 'Z') t |= 0x1 | 0x100; if (ch >= 'a' && ch <= 'z') t |= 0x2 | 0x100;
        if (ch >= '0' && ch <= '9') t |= 0x4; if (ch == ' ' || (ch >= 9 && ch <= 13)) t |= 0x8;
        if (ch == ' ' || ch == '\t') t |= 0x40; if (ch < 32 || ch == 127) t |= 0x20;
        if (ch > 32 && ch < 127 && !(t & 0x107)) t |= 0x10; if (ch >= '0' && ch <= '9') t |= 0x80;
        if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) t |= 0x80;
        WR16(A(4) + 2 * i, t);
    }
    return 1;
}
static uint32_t x_GetStringTypeW(CPU *c, const uint32_t *a) {   /* (type, src, n, out) */
    int n = (int)A(2);
    if (n < 0) { n = 0; while (RD16(A(1) + 2 * n)) n++; n++; }
    uint8_t tmp[1024]; if (n > 1024) n = 1024;
    for (int i = 0; i < n; i++) { uint16_t w = RD16(A(1) + 2 * i); tmp[i] = w < 256 ? (uint8_t)w : '?'; }
    uint32_t buf = galloc((uint32_t)n + 1); memcpy(G + buf, tmp, (size_t)n);
    uint32_t args[5] = {0, A(0), buf, (uint32_t)n, A(3)};
    x_GetStringTypeA(c, args); gfree(buf);
    return 1;
}
static uint32_t x_LCMapStringA(CPU *c, const uint32_t *a) {     /* (lcid, flags, src, n, dst, dn) */
    int n = (int)A(3); const char *s = HSTR(A(2));
    if (n < 0) n = (int)strlen(s) + 1;
    if (!A(5)) return (uint32_t)n;
    if ((uint32_t)n > A(5)) return 0;
    for (int i = 0; i < n; i++) { char ch = s[i];
        if (A(1) & 0x100) ch = (char)tolower((unsigned char)ch); else if (A(1) & 0x200) ch = (char)toupper((unsigned char)ch);
        WR8(A(4) + i, (uint8_t)ch); }
    return (uint32_t)n;
}
static uint32_t x_LCMapStringW(CPU *c, const uint32_t *a) {
    int n = (int)A(3);
    if (n < 0) { n = 0; while (RD16(A(2) + 2 * n)) n++; n++; }
    if (!A(5)) return (uint32_t)n;
    if ((uint32_t)n > A(5)) return 0;
    for (int i = 0; i < n; i++) { uint16_t w = RD16(A(2) + 2 * i);
        if (w < 128) { if (A(1) & 0x100) w = (uint16_t)tolower(w); else if (A(1) & 0x200) w = (uint16_t)toupper(w); }
        WR16(A(4) + 2 * i, w); }
    return (uint32_t)n;
}
static uint32_t x_CompareStringA(CPU *c, const uint32_t *a) {   /* (lcid, flags, s1, n1, s2, n2) */
    const char *s1 = HSTR(A(2)), *s2 = HSTR(A(4));
    size_t n1 = (int)A(3) < 0 ? strlen(s1) : A(3), n2 = (int)A(5) < 0 ? strlen(s2) : A(5);
    size_t n = n1 < n2 ? n1 : n2;
    int r = (A(1) & 1) ? strncasecmp(s1, s2, n) : strncmp(s1, s2, n);
    if (!r) r = n1 < n2 ? -1 : n1 > n2;
    return r < 0 ? 1 : r > 0 ? 3 : 2;
}
static uint32_t x_CompareStringW(CPU *c, const uint32_t *a) {
    uint32_t p1 = A(2), p2 = A(4); int n1 = (int)A(3), n2 = (int)A(5);
    if (n1 < 0) { n1 = 0; while (RD16(p1 + 2 * n1)) n1++; }
    if (n2 < 0) { n2 = 0; while (RD16(p2 + 2 * n2)) n2++; }
    for (int i = 0; i < n1 && i < n2; i++) {
        uint16_t x = RD16(p1 + 2 * i), y = RD16(p2 + 2 * i);
        if ((A(1) & 1) && x < 128 && y < 128) { x = (uint16_t)tolower(x); y = (uint16_t)tolower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return n1 < n2 ? 1 : n1 > n2 ? 3 : 2;
}
/* the MSVC CRT asks for SSE/SSE2 to pick memcpy/math paths: say no, keep it on plain x86 */
static uint32_t x_IsProcessorFeaturePresent(CPU *c, const uint32_t *a) { (void)a; return 0; }
static uint32_t x_GetWindowThreadProcessId(CPU *c, const uint32_t *a) { if (A(1)) WR32(A(1), 1); return 1; }

#define X(dll, n, args) {dll, #n, args, x_##n}
const ImportDef imp_extra[] = {
    X("KERNEL32.DLL", CompareFileTime, 2), X("KERNEL32.DLL", CreateProcessA, 10),
    X("KERNEL32.DLL", GetDriveTypeA, 1), X("KERNEL32.DLL", GetExitCodeProcess, 2),
    X("KERNEL32.DLL", GetFileTime, 4), X("KERNEL32.DLL", GetLogicalDrives, 0),
    X("KERNEL32.DLL", GetLogicalDriveStringsA, 2), X("KERNEL32.DLL", GetStartupInfoA, 1),
    X("KERNEL32.DLL", GlobalAlloc, 2), X("KERNEL32.DLL", GlobalFree, 1),
    X("KERNEL32.DLL", OpenMutexA, 3), X("KERNEL32.DLL", RaiseException, 4),
    X("KERNEL32.DLL", SetProcessAffinityMask, 2), X("KERNEL32.DLL", WinExec, 2),
    X("KERNEL32.DLL", WritePrivateProfileStringA, 4), X("KERNEL32.DLL", _lopen, 2),
    X("KERNEL32.DLL", _lclose, 1),
    X("USER32.DLL", LoadCursorA, 2), X("USER32.DLL", UnregisterClassA, 2),
    X("USER32.DLL", DdeClientTransaction, 8),
    X("ADVAPI32.DLL", RegSetValueExA, 6),
    X("VERSION.DLL", GetFileVersionInfoSizeA, 2), X("VERSION.DLL", GetFileVersionInfoA, 4),
    X("VERSION.DLL", VerQueryValueA, 4),
    X("WINMM.DLL", mciSendStringA, 4), X("WINMM.DLL", mixerGetNumDevs, 0),
    {"WINMM.DLL", "mixerGetLineInfoA", 3, x_mixer_fail}, {"WINMM.DLL", "mixerGetLineControlsA", 3, x_mixer_fail},
    {"WINMM.DLL", "mixerGetControlDetailsA", 3, x_mixer_fail}, {"WINMM.DLL", "mixerSetControlDetails", 3, x_mixer_fail},
    X("KERNEL32.DLL", HeapCreate, 3), X("KERNEL32.DLL", HeapDestroy, 1), X("KERNEL32.DLL", SetHandleCount, 1),
    X("KERNEL32.DLL", InterlockedIncrement, 1), X("KERNEL32.DLL", InterlockedDecrement, 1),
    X("KERNEL32.DLL", TerminateProcess, 2), X("KERNEL32.DLL", GetVersionExA, 1),
    X("KERNEL32.DLL", GetEnvironmentVariableA, 3), X("KERNEL32.DLL", GetEnvironmentStringsW, 0),
    X("KERNEL32.DLL", FreeEnvironmentStringsW, 1), X("KERNEL32.DLL", GetStringTypeA, 5),
    X("KERNEL32.DLL", GetStringTypeW, 4), X("KERNEL32.DLL", LCMapStringA, 6), X("KERNEL32.DLL", LCMapStringW, 6),
    X("KERNEL32.DLL", CompareStringA, 6), X("KERNEL32.DLL", CompareStringW, 6),
    X("KERNEL32.DLL", IsProcessorFeaturePresent, 1), X("USER32.DLL", GetWindowThreadProcessId, 2),
    {0},
};
