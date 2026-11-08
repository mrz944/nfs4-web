# NFS4 Web

**Need for Speed: High Stakes (PC, 1999), recompiled to run in your browser, with online multiplayer.**

The same approach as [NFS3 Web](https://github.com/mrz944/nfs3-web): the game's own x86 code
is statically recompiled to C and then WebAssembly. It runs the real game logic, physics, AI
and netcode. Windows, DirectX, the Voodoo2 (Glide 3) graphics card and the network stack are
replaced by a small runtime written for the web. Most of that runtime is shared with NFS3 Web.

The game runs the **[NFS4 Modern Patch](https://veg.by/en/projects/nfs4/)** by VEG (based on 4.50).
Its "thrash" renderer architecture is the same as NFS3's, including a Glide 3 `voodoo2a.dll`
driver. The game calls exactly the same 55 Glide functions, so the Glide → WebGL 2 renderer
carries over unchanged.

## What's new compared with NFS3 Web

| | |
|---|---|
| **Lifter** | NFS4 uses instructions NFS3 didn't: MMX (movie decoder, blitters), `cmov`, `fcomi`, `ffree`, `fsincos`, `enter`. |
| **Win32** | ~50 more imports (`runtime/win32_extra.c`): drives and CD probing, child processes, version info, mixer and CD audio, `WritePrivateProfileString`, and the MSVC CRT start-up of the patched `voodoo2a.dll`. `LoadLibrary("DDRAW.DLL")` etc. now hand out the runtime's own system DLLs. |
| **Single-core mode** | NFS4 is not safe on a multi-core CPU. That's why the modern patch has `SingleProcAffinity=1`. Its file streamer, for example, checks a chunk before locking it (abort: *"STREAM_release - chunk has already been released"*), and busy-waits for its reader thread without calling the OS. The runtime emulates one core: only one guest thread runs at a time. It is handed over at blocking API calls, by thread priority as on Windows, with a 4 ms time slice among equals. The lifter puts a preemption check (one flag test) on every loop back-edge so that busy-waits hand over too. `RT_MULTICORE=1` turns it off. |
| **Online** | NFS4's IPX mode runs on the same virtual LAN. NFS4 adds a choice that NFS3 didn't have: **Positional Updates** vs **Controller Updates** (see below). |
| **Page** | Restyled after NFS4's own front end. The lobby name is written into `data/TEXT/TEXT.ENG` (default name) and `savedata/config.dat` (offset `0xF2C`). |

## Online play: how NFS4 does it

Connect Players offers *EA Racing Online* (EA's NPS lobby service, long shut down), *Network (IPX)*,
*Network (TCP)*, *Modem* and *Serial*. The browser build uses **IPX**, exactly as NFS3 Web does:
a room on the server looks like a LAN to the game. Every player gets a virtual IPX node, game
discovery is the game's own SAP broadcast on socket `0x452`, and packets go browser to browser
over WebRTC with a relay fallback.

When hosting, NFS4 asks:

- **Peer-to-Peer** or **Client/Server**: the topology, as in NFS3.
- **Controller Updates**: NFS3-style lockstep. Every machine simulates all cars from the
  inputs, the host's input counts at once, and clients wait one round trip to the host.
- **Positional Updates**: each machine drives its own car and sends its position. Your own
  car reacts instantly whoever hosts, which makes it the better choice over the internet.

Race traffic: Controller Updates send 32 small packets/s each way, as in NFS3. Positional Updates
send about 8 position packets/s (~240 bytes) and each game simulates its own car. Tested natively with
`RT_NET_DELAY=75` on both sides (150 ms round trip): a two-player race runs with matching clocks and no drops.

**Verified natively** (`tools/nettest.sh`, two headless instances through the relay): the game
list shows the host's game, the client joins, both show up in the multiplayer lobby with live
delay bars, and a two-player race on Celtic Ruins runs on both with synchronised clocks and
shared AI, with 0 drops.

## Graphics modes

NFS4 lists only exact 4:3 modes (`nfs4.exe` 0x51a4a5 checks `w * 3 == h * 4`) and renders in 4:3, so
real 16:9 would need a field-of-view rework of the game. Instead the driver's 16 mode records
(rewritten when `voodoo2a.dll` loads, `glide_patch_driver`) carry sharp 4:3 modes up to
**2880x2160**; the page pillarboxes them. With a Voodoo driver the game fixes its video memory
budget at 8 MB (only modes up to 1600x1200 fit); the runtime raises it to the renderer's 32 MB.
The page passes the screen size (`RT_SCREEN`), so only modes that fit the screen are offered.
Pick one in **Options → Graphics → Screen Size**: the menus stay 640x480, races use the chosen
mode. Tested natively: a race at 1792x1344 runs at the game's 64 fps cap.

Also from NFS3 Web: full frame rate on Windows (timers woken from the audio thread instead of
1 ms sleeps that Windows browsers round up to 15.6 ms), and Exit Game returns to the start page.

## Status

- 🌐 Public at **https://nfs4.xyz** via a Cloudflare Tunnel (page, lobby, relay). No game files are hosted: players pick their own NFS4 folder with the Modern Patch beta. See [docs/cloudflare.md](docs/cloudflare.md).

- ✅ Native build: intro movie, front end, car showcase (3D), single race with 7 AI cars, keyboard and mouse
- ✅ Online, native: IPX discovery, host/join, multiplayer lobby, two-player race
- ✅ Browser build compiles (`web/dist/nfs4.{js,wasm}`, 13.5 MB wasm)
- 🚧 Not yet tested in a real browser (the build machine's sandbox blocks browsers from local servers)
- 🚧 Hot Pursuit / High Stakes / Career modes not yet played through; sound only checked natively

## Getting started

You need a copy of **NFS: High Stakes** (or *Road Challenge*) for PC plus the modern patch.

```sh
python3 -m venv .venv && .venv/bin/pip install capstone pefile

# game folder: DATA and SAVEDATA from the CD, then the modern patch on top
mkdir game && cp -r /cd/DATA game/data && cp -r /cd/SAVEDATA game/savedata
7z x nfs4_modern_patch_beta.7z -ogame -y          # https://veg.by/files/nfs4/
# merge the patch's data/menus into the CD's data/MENUS (same files, different case)

./build.sh game        # lift nfs4.exe, eacsnd.dll, voodoo2a.dll + native Linux build
./build-web.sh game    # browser build → web/dist/nfs4.{js,wasm}
node web/serve.mjs 8080 --game game
```

A CloneCD image (`.img`) can be turned into an ISO with `tools/img2iso.py in.img out.iso`.
The page patches `nfs4.ini` in each browser: `ThrashDriver=glide3x`, `NoMovies=1`, `SingleProcAffinity=1`.

### Native test helpers

| | |
|---|---|
| `tools/race.sh DIR SECS` | headless single race by mouse clicks (start-up smoke test) |
| `tools/nettest.sh SOCK SECS "IN_A" "IN_B"` | two instances in one relay room (`node web/serve.mjs 8095 --relay-tcp build/relay.sock`) |
| `RT_CURSOR_ADDR=689510` | scripted `CLICK@x;y` move from the game's real cursor (NFS4 moves it to the focused button itself) |
| `RT_INPUT=…,MEMDUMP` | dump the exe's data/bss to `build/mem_<t>.bin` (reverse-engineering aid) |
| `RT_MULTICORE=1` | turn off single-core mode |
| `RT_NO_DIRCACHE=1` | turn off the folder-listing cache |
| `RT_GUARD=1` | fault on guest accesses past 512 MB, like the browser (natively they silently read zeros) |
| `RT_NET_DELAY=ms` · `RT_FPS_LOG=1` · `RT_TICK_MS=15.6` | simulated latency · fps log · Windows-like coarse timers |

Everything from NFS3 Web still applies (`RT_HEADLESS`, `RT_DUMP`, `RT_TRACE`, `RT_NET*`, `RT_PROF`, `./re`).

## Credits

- [NFS3 Web](https://github.com/mrz944/nfs3-web) by Marcin Zadka: the runtime, lifter, renderer, lobby and relay this is built on.
- [NFS4 Modern Patch](https://veg.by/en/projects/nfs4/) by VEG (Evgeny Vrublevsky).
- [Capstone](https://www.capstone-engine.org/) and [Emscripten](https://emscripten.org/).
- Site font: [Russo One](https://fonts.google.com/specimen/Russo+One) (SIL OFL, `web/fonts/`).

*Need for Speed* and *High Stakes* are trademarks of Electronic Arts. This is an unofficial fan
project, not affiliated with or endorsed by EA. Game files, generated C and the wasm build are
never committed (see `.gitignore`).
