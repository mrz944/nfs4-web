# nfs4-webgl — architecture

Static recompilation of *Need for Speed: High Stakes* (PC, modern patch) to WebAssembly + WebGL 2.
Forked from [NFS3 Web](https://github.com/mrz944/nfs3-web). The lifter, runtime, Glide renderer, lobby and relay are the same code. This file covers
what's different for NFS4. See NFS3 Web's ARCHITECTURE.md for the shared design (lifter,
register promotion, Glide frame recording, OPFS, WebRTC links).

## Target binaries (analyzed 2026-11-07)

| Module | Base (lifted at) | Size | SHA-256 |
|---|---|---|---|
| `nfs4.exe` (modern patch beta, from nfshs.exe 4.50, built Feb 16 2001) | `0x400000` | image `0x63B000`, entry `0x55A480` | `e00d2037…68bb45fd` |
| `eacsnd.dll` (EA sound, Watcom) | `0xB00000` (rebased) | `0x10000` | `b23b61b8…2163c9a358b` |
| `drivers/glide3x/voodoo2a.dll` (thrash driver, MSVC) | `0xC00000` (rebased) | `0x1D000` | `e05aed62…dd9668b67` |

`softtria.dll` isn't lifted: its 6.8 MB bss would overlap the guest heap at `0x01000000`, and
the Glide driver is the only one the browser build uses.

Lifted: 7,498 functions / 763k instructions (exe), 241 (eacsnd), 690 (voodoo2a). Unsupported
instructions remaining: 244 in the exe, all in data the discoverer decodes as code (`in`, `aaa`,
`salc`, `bound`, `enter` with a nesting level, …).

### Imports: 224 in the exe, 192 shared with NFS3

New: `DDRAW` (`DirectDrawCreate`/`Enumerate`, the "DirectX 6 installed?" probe through
`LoadLibrary`), `VERSION` (3), mixer and MCI from `WINMM` (CD audio volume), drives
(`GetLogicalDrives`, `GetDriveTypeA`), `CreateProcessA`/`WinExec` (self-restart),
`WritePrivateProfileStringA` (the patch keeps settings in `nfs4.ini`), `_lopen`/`_lclose`,
`GlobalAlloc`, `OpenMutexA`. The MSVC-built `voodoo2a.dll` adds the CRT start-up set
(`HeapCreate`, `GetVersionExA`, `LCMapString*`, `GetStringType*`, `CompareString*`, …), and
`eacsnd.dll` imports `DSOUND.DLL` by ordinal 1 (`DirectSoundCreate`).

## Game layout

```
nfs4.exe  nfs4.ini  eacsnd.dll
drivers/<name>/thrash.ini + *.dll     ThrashDriver=<name> picks one (glide3x for us)
data/     CARS  TRACKS  FEART  GAMEART  MENUS  AUDIO  MOVIES  SHOWCASE  TEXT  PLAC.VIV
savedata/ config.dat  DB/*.CDB  CARSPECS/  CARS/  RECORDS.DAT     (written by the game)
```

The modern patch's `data/menus` must be merged into the CD's `data/MENUS`. Lookups are
case-insensitive per path component, and two folders that differ only in case would hide one of them.

## Single-core mode (runtime/core.c)

NFS4 was written for one CPU. The patch's `SingleProcAffinity=1` pins it to one core on Windows.
Two bugs show up with truly parallel threads:

1. `STREAM_release` (0x52E200) tests a chunk's state *before* `EnterCriticalSection`, so the
   reader thread and the game thread can both release it → abort "chunk has already been released".
2. The game thread busy-waits at 0x52E1AB for the streamer to tag a chunk, with no OS call in
   the loop. On a single core the scheduler's timer preempts it.

The runtime emulates one core:

- a lock (`bgl_*`) that a guest thread holds while it runs guest code. Like Windows' scheduler,
  it goes to the waiting thread with the highest `SetThreadPriority` (FIFO among equals). A
  thread that has waited 50 ms goes first, as Windows boosts starved threads;
- thunks whose names can block (`Wait*`, `Sleep*`, `GetMessage`, `recv*`, `select`,
  `ExitThread`, `grBufferSwap`, …) drop it around the call. `EnterCriticalSection` drops it
  only when the section is actually taken;
- any other API call, and any loop back-edge, hands over at once if a higher-priority thread
  waits, otherwise after a 4 ms slice if anyone does;
- `guest_call` acquires it, which covers host threads entering guest code (new threads, timer callbacks);
- every lifted loop back-edge does `if (g_bgl_contended) bgl_preempt();`. That's one flag read
  in the common case, and it's what lets busy-waits yield.

File I/O (`ReadFile`, `WriteFile`, `CreateFileA`) also drops the lock, as a disk wait would on Windows.

The thread priorities NFS4 sets: the 128 Hz service thread that runs its timer "interrupt"
(audio, streaming) is time-critical (15), the game clock thread 2, the menu loader 1, and the
game thread normal (0). Loading is paced by that service: one ~8 KB read per 7.8 ms tick.
Before this scheduling, the loader and timer threads each waited a full 4 ms slice behind the
game thread at every critical section, which stretched a menu screen change to 2–3 s.

The front end also has deliberate waits that draw nothing: the title screen waits 3 s for a key
(`0x4c2788`: 384 ticks), and a loop like it runs before a race.

**Stall log.** A frame that takes over 300 ms logs where the time went, per API and thread
(`stall: frame took … | t118 KERNEL32.DLL!EnterCriticalSection 2665ms/940 | …`). This is always on and
also shows in the browser's client logs.

## File lookups (runtime/kernel32.c)

Guest paths are matched case-insensitively per component. In the browser a `readdir` of an
OPFS folder costs ~30 ms, and the front end probes ~180 showcase files that don't exist
(`Data\Showcase\Art\HPP91.qfs`, …), which stalled menus for seconds. Folder listings are
cached and re-read only after the runtime itself creates, deletes or renames something in that
folder (`fs_changed`). Opening a file known to be missing makes no host call. `RT_NO_DIRCACHE=1` turns the cache off.

## Online (IPX)

Unchanged runtime (`runtime/net.c`, `web/relay.mjs`, `web/net.js`). Observed with
`RT_NET_TRACE`: game discovery is a 4-byte SAP query broadcast to socket `0x452` from socket
`0x4001`. In a race both sides send 32 packets/s. The host's Game dialog chooses topology
(Peer-to-Peer / Client/Server) and netcode (Positional / Controller updates); see README.

## Notes for scripted tests

- The game boots into the Load screen (no saved player yet). Cancel → main menu.
- NFS4 moves its cursor onto the focused button, so relative mouse scripts drift.
  `RT_CURSOR_ADDR=689510` (cursor x, y as int32, a mirror at `0x821B30`) makes `CLICK@x;y`
  start from the real position.
- Some screens only accept a click after the cursor has hovered for a moment: `MOVE@`, then `CLICK@…+300`.
- Player name: `data/TEXT/TEXT.ENG` string table entry "Player1"; `savedata/config.dat` 16 bytes at `0xF2C`.
