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

The runtime keeps the current folder (`SetCurrentDirectoryA`), as in NFS3 Web: relative paths in
the file calls start there, DLLs and INI files from the game folder. NFS4 itself builds full save
paths (`%sSaves\*.%s`) and hasn't been seen changing folders.

## Online (IPX)

Unchanged runtime (`runtime/net.c`, `web/relay.mjs`, `web/net.js`). Observed with
`RT_NET_TRACE`: game discovery is a 4-byte SAP query broadcast to socket `0x452` from socket
`0x4001`. In a race both sides send 32 packets/s. The host's Game dialog chooses topology
(Peer-to-Peer / Client/Server) and netcode (Positional / Controller updates); see README.

A link that fails or isn't up after 20 s logs what ICE had (candidate types per side, how the
pairs ended, TURN errors; never addresses), and every link logs the path it took. Joining waits
up to 2 s for the TURN servers. `?icetest` probes each STUN and TURN server on its own.

## Players (web/players.mjs)

Players without accounts, the same as in NFS3 Web. Every browser makes a player key: 8 words from
the EFF short word list (`web/words.js`), kept in its storage. The server gets only a hash of the
words (`SHA-256("nfs4 player v1:" + words)`, hashed again), and gives the player a random public
id. The same words on another device are the same player there (*Your Passphrase*).

- A name belongs to one player: compared without case or look-alike digits, held for 7 days
  after a change, 3 changes a day. The relay refuses a name another player owns.
- Lap records and the most active racers are one row per player, under the current name.
- Analytics events, play and connection logs, room logs and session logs carry the player id.
  `/__stats` has a Players section with a lookup; `tools/players.mjs` does the same from the
  terminal and fixes names and laps by hand.

## Lap records (runtime/game.c, web/laps.mjs, web/replays.mjs)

Found with `RT_INPUT=…,MEMDUMP` dumps during native races: the AI cars' lap times stepped from 0
to about 100 s worth of 1/64 s ticks at fixed places 0xad4 bytes apart, and the code that copies
those records led to their pointer table.

- Cars: pointers at `0x661a88` to records of 0xad4 bytes. In a record: `+0x1dc` the car's folder
  code (`P911`), `+0x1ec` the human's number (-1 the AI), `+0x1f8` 5 human / 11 AI, plus 0x200
  when another machine drives the car, `+0x22c` laps finished, `+0x238` each lap's time
  (1/64 s, up to 16 laps).
- The local player's car: a human's car no other machine drives. Online every machine has every
  human's car with the same numbers (host 0, client 1), so `+0x1ec` alone doesn't say whose.
- Race settings: the game's settings block at `0x7c3c04` (the copy its code reads; others sit at
  `0x7421b0`, `0x8295ec`, `0x853b9c`). Found by toggling each Location option between dumps:
  track `+0x10e0` (an index into the track folder table at `0x5ba21c`: 0 Hills = Celtic Ruins,
  … 18 Empire City), laps `+0x10e8`, backward `+0x10ec`, mirrored `+0x10f0`, night `+0x10f4`,
  weather `+0x10f8`.
- Replays: at the end of every offline race the game writes `SaveData\replay.rp4`, which starts
  with a copy of that settings block (0x18a4 bytes, its first word). Load › Replays lists
  `SaveData\Saves\*.rp4`. The page uploads `replay.rp4` after a race whose laps it sent, and
  puts a record's replay into `savedata/Saves`.
- A replay playing (Load › Replays, or Replay after a race) isn't a race: its cars drive their
  laps again. The replay state is `0x7c4be4` (settings block `+0xfe0`): the replay loader sets it
  to 2 when a replay file loads (`0x512723`), 0 in a race and in the menus. No laps are sent
  while it is 2 or more. The server also drops a race whose every lap equals, to the tick, an
  earlier race's lap with the same track, settings and car (a replay sent by an older build).
- Checked natively with `RT_AUTOPILOT=1` (the game's AI drives the player's car): laps, track
  and all four flags offline, and online with two instances. An autopilot race's replay has no
  recording (the human control routine records the player's car), so playing a real replay back
  needs a human-driven race.

## Notes for scripted tests

- The game boots into the Load screen (no saved player yet). Cancel → main menu.
- NFS4 moves its cursor onto the focused button, so relative mouse scripts drift.
  `RT_CURSOR_ADDR=689510` (cursor x, y as int32, a mirror at `0x821B30`) makes `CLICK@x;y`
  start from the real position.
- Some screens only accept a click after the cursor has hovered for a moment: `MOVE@`, then `CLICK@…+300`.
- Player name: `data/TEXT/TEXT.ENG` string table entry "Player1"; `savedata/config.dat` 16 bytes at `0xF2C`.
