# NFS4 Web widescreen patch: plan

Branch `widescreen-patch`, staging worktree `~/DEV/nfs4-webgl-staging`, tested on staging.nfs4.xyz.
Read CLAUDE.md (rules, ports, harness) first. Work phase by phase; each phase ends with a native
check, then a staging check by the user where it says so. Tick the status table as you go and add
what you found (addresses, surprises) to the phase's **Findings** line, so the next session
starts from there.

## Goal

One patch, in our own code, that makes NFS4 Web:

1. **Real widescreen in races**: 16:9 and 16:10 modes (21:9 if the game copes) with Hor+ (the
   same vertical view as 4:3, more on the sides), nothing culled or stretched at the edges, the
   HUD at the right proportions and anchored to the screen edges. Menus stay 640x480, pillarboxed.
2. **What the two mods from GitHub issue (De-M-oN) do that still matters in a browser**, done
   our way: the Vulkan Thrash driver's widescreen and frame-rate changes, and HS Music And Tweaks'
   faster loading and interactive music. Their binaries are never shipped (see "Mods" below).
3. **Optionally, a faster Positional Updates rate online** (8 → 32 Hz), only if it stays
   compatible (P10).

A player with an old config.dat keeps their 4:3 mode; nothing changes until they pick a wide mode.

## Status

| Phase | What | State |
|---|---|---|
| P0 | Staging up, baseline screenshots | ☑ 2026-11-27 |
| P1 | Hooks: lifted functions replaced by runtime C | ☑ 2026-11-27 |
| P2 | Wide modes offered and selectable | ☑ 2026-11-27 |
| P3 | Hor+ projection | ☑ 2026-11-27 (replay TV cameras: check in P6) |
| P4 | Culling, clipping, effects at the new edges | ☑ 2026-11-28 (nothing to change) |
| P5 | HUD and 2D overlays | ☐ |
| P6 | Mirror, split screen, replays, pause and race-end screens | ☐ |
| P7 | Page, defaults, settings migration | ☐ |
| P8 | Frame rate and pacing (Vulkan Thrash) | ☐ |
| P9 | Mods analysis and the features worth taking (HS Tweaks) | ☐ (needs the downloads) |
| P10 | Positional Updates 8 → 32 Hz (optional) | ☐ |
| P11 | Docs, changelog, staging test, merge | ☐ |

## How changes go into the game (the design rule)

The game is lifted to C ahead of time (`tools/lift.py` → `gen/`). Writing to code bytes at run
time does nothing (no interpreter; `VirtualProtect` is a stub), and the player's own `nfs4.exe`
stays unpatched (the page checks its SHA-256 against `web/dist/build-info.json`, which
`build-web.sh` takes from `game/nfs4.exe`). So:

- **Code changes** are hooks (P1): the lifter emits a call to our C function in place of a
  lifted function, and the original stays callable. Our C reads the setting at run time, so wide
  and 4:3 behaviour live in one build. Prefer this over byte patches.
- **Small code changes inside a big function** (a branch, a constant): a byte patch list applied
  to the image the *lifter* reads (never to `game/nfs4.exe` on disk), each entry checked against
  the original bytes. Use only where a hook would mean rewriting a large function.
- **Data changes** (tables, constants in `.data`): written into guest memory after the exe loads,
  checked against the original bytes, like `nfs4_vram_budget` in `runtime/glide.c`.
- **Renderer-side changes** (letterbox, page) in `runtime/gpu.c`, `runtime/glide.c`, `web/`.

Nothing here changes game logic, so Hor+ and the HUD work need no online version gating. P10 does.

## Known addresses (from the 2026-11 analysis; check them before relying on them)

| What | Where |
|---|---|
| Settings block (config.dat, replays) | `0x7c3c04`, `0x18a4` bytes; `winsize` `0x7c3d20`, width `0x7c3d24`, depth `0x7c3d28`, `tripplebuffer` `0x7c3d54` |
| Mode records (driver) | via `[0x9e801c]` → info struct, count `+0x3c`, records `+0x40`, 40 bytes each `{w, h, bpp, 4, available, colorBuffers, …}`; rewritten by `glide_patch_driver` (16 slots, codes map `{7,8,9,12,13,14,0,1,2,15,11,3,4,5,6,10}`) |
| 4:3 filter, options list | around `0x51a499`–`0x51a4b5` (`w*3 == h*4`; match → `0x51a3ba`, else skip → `0x51a3e8`) |
| Mode lookup | `f_0051A530` (eax = width, edx = depth), called from `0x4c261c`, `0x4c2bba`; same 4:3 test; no mode → "AMF=5 screen.c(563)" |
| Video memory budget | `0x5befa4` (runtime raises it to 32 MB) |
| View setup | `f_00453F10(x, y, w, h, fov, ?, flip)` (`ret 0x1c`): half sizes `0x5b0958`/`0x5b095c` = w/2, h/2; centre `0x680f98`/`0x680f9c`; focal `0x5b0950` = (w / tan(fov·k)) / 2 (k at `0x5938d1`, 0.5 at `0x5938cd`), `0x5b0954` = −focal. **Focal from the width = fixed horizontal FOV = Vert− on a wide screen: this is the Hor+ fix point.** |
| View callers | `0x539f23` (in the function ending `0x539f2b`); per-view records `0x853a10` (main), `0x853a30 + 32·player` (split screen, when `[0x7c4bec] == 1`), `0x853a70`; `f_0051A2C0` sets a view from a record; `0x53a043`/`0x53a06f` read the half sizes again (likely the clip or cull setup) |
| Viewport rect | `f_00453E60` (called from `0x51a21d`) stores the rect at `0x680f70`–`0x680fa0` |
| Cars, laps, race settings | see ARCHITECTURE.md "Lap records" |

The lifted C has each instruction as a comment (`/* 00453F10 push ebp */`), so
`grep -n "/\* 00453F" gen/nfs4/code_*.c` reads the disassembly.

## P0: staging up, baseline

- The user: `tools/staging.sh tunnel-setup` once, then `tools/staging.sh server` and
  `tools/staging.sh tunnel`; check https://staging.nfs4.xyz shows the password page, then the game.
- Native baseline: `tools/race.sh build/p0-43 90` at a 4:3 mode, keep a few frames as PNG
  (`build/baseline/`) for later comparison: start grid, a corner, the HUD, the mirror.
- Make a config.dat per test mode (from a `MEMDUMP` of the settings block or `build/re/menu.bin`,
  width/depth changed) so each test starts in the mode it needs.

**Done when**: staging serves the current game; baseline frames saved. **Findings** (2026-11-27):

- Staging: server (8085) and tunnel "nfs4-staging" up; https://staging.nfs4.xyz answers with the
  password page (401), and with the cookie serves the page (STAGING ribbon), `build-info.json`
  and `game/` through `__dev/game/` (401 without it). A browser run of the game: the user.
- Baseline: `build/baseline/` (PNGs + README.txt: how each was made). Test configs:
  `build/configs/` (README.txt). Race memory dump: `build/p0-race1280.bin` (MEMDUMP, base `0x591000`).
- **The game already has a letterbox: Options › Graphics › Wide Screen**, `0x7c3d5c` (settings
  block `+0x158`), **on by default** (set to 1 at `0x4c25cb`). With it on, the main view record
  `0x853a10` is `{x 0, y −80, w 1280, h 960, clip 0, 160, 1280, 640}` at 1280x960: the 4:3
  projection (centre moved up h/12) cropped to a centred 2:1 band, the HUD on the black bars.
  Off: `{0, 0, 1280, 960, 0, 0, 1280, 960}`, a full 4:3 view. The band is set in `f_005197D0`
  and around `0x51990b`/`0x519ca7` (clip height = `w/2 · [0x853b5c]/128`; `0x853b5c` animates
  the bars in, ≤ 0x80, then sits high, 987 in a race). So most players race at 2:1 Vert−
  today. P3 must decide what Wide Screen means on a 16:9 frame (likely: Hor+ full frame, and the
  option either kept as a 2:1 crop of the wide view or ignored).
- The same menu has **View Angle** and **Perspective** settings: look at them in P3 (FOV inputs).
- Other records at 1280x960: split screen `0x853a30` `{0,−240,1280,960, 0,0,1280,480}`,
  `0x853a50` `{0,240,1280,960, 0,480,1280,480}` (each half keeps the full 4:3 projection,
  cropped); `0x853a70` (mirror?) `{400,−74,385,240, 400,6,385,133}`. Half sizes 640/480, focal
  500.02, centre (640, 400) with Wide Screen on.
- No mirror showed in the four cameras (C cycles cockpit → chase → far chase → bumper; R, F1
  did nothing visible): find the mirror's option in P6.
- Harness: headless dumps were always 640x480 (`gpu_egl.c`); they now follow the frame,
  `RT_DUMP_H=H` (default 480, width by aspect; `0` = native size). `RT_AUTOPILOT=1` gives real
  driving. A held key in `RT_INPUT` delays every later event until it is released.

## P1: hooks

Add a hook list to the lifter, e.g. `runtime/hooks.txt` (`<address> <c name>`), passed by
`build.sh` (`--hooks`). For each listed function the lifter:

- emits the lifted body as `f_<addr>_orig` (unchanged code), and
- emits `f_<addr>(c)` as a call to `hook_<name>(c)` (declared in `runtime/lifted.h` or a new
  `runtime/hooks.h`), so every direct call and the dispatch table reach the hook.

The hook gets the guest CPU (`c`), reads arguments from the guest stack as the original would,
and either calls `f_<addr>_orig(c)` or does the work itself. Mind the calling convention (Watcom
register args: eax, edx, ebx, ecx, then stack; callee pops with `ret N`) and the x87 stack if
the function returns a float.

- New file `runtime/widescreen.c` for the hooks of this patch.
- A `RELIFT=1 ./build.sh game` is needed whenever the list changes; note that in build.sh.
- Check: a pass-through hook on `0x453f10` gives a race identical to the baseline (frames byte
  for byte with the same input script, or as close as the timing allows).

**Done when**: hooks work, documented in ARCHITECTURE.md. **Findings** (2026-11-27):

- Done as planned: `runtime/hooks.txt`, `tools/lift.py --hooks`, `runtime/hooks.h`
  (`HOOK_ARG32`/`HOOK_ARGF`), `runtime/widescreen.c` (`hook_view_setup`, pass-through, with
  `RT_WS_LOG=1`). `build.sh` re-lifts by itself when the list differs from `gen/<m>/hooks.txt`
  (no RELIFT needed; ~1 min for the three modules); `build-web.sh` now always goes through
  `LIFT_ONLY=1 ./build.sh`. ARCHITECTURE.md "Hooks".
- The relifted `gen/nfs4` differs from the old one only in the hooked function's name, the
  `f_00453F10` wrapper and two declarations (the lifter is deterministic).
- Byte-identical frames are not possible: two runs of the same binary differ from the loading
  screen on (wall-clock timing). The check used instead: a race dump at the same spot before and
  after the hook has the same focal, half sizes, centre and view record. The race runs normally.
- Calls seen (`RT_WS_LOG=1`, 1280x960): one caller, `0x539f28`. Front end `(0,0,640,480, fov 30, n 24)`.
  Race: `fov 52` with Wide Screen on (y −80), `fov 45` with it off, plus a `fov 40` view in the
  intro. The same call for all four cameras (cockpit, chase, far chase, bumper). `k` at
  `0x5938d1` is π/180: fov is the half-angle in degrees (52 → focal 500 at w 1280, i.e. ±52°
  across the width). `n` ≈ fov·0.8, returned in eax and bumped by 1 on the stack (looks like a
  leftover). So the letterbox widens the horizontal half-angle 45 → 52 while cropping to 2:1.

## P2: wide modes

- `glide_patch_driver` (`runtime/glide.c`): the 16 record slots carry sharp 4:3 sizes today.
  Keep the codes old configs use (7 640x480, 8 800x600, 10 856x480, 12 1024x768, 14 1600x1200)
  and give the rest a mix of 4:3 and wide sizes: 1280x720, 1600x900, 1920x1080, 2560x1440,
  3840x2160, 1920x1200, 2560x1600, and 2560x1080/3440x1440 if 21:9 works. Check whether the
  record count can grow beyond 16 (who allocates the records, what reads `+0x3c`) before
  dropping 4:3 sizes; mind the VRAM budget check right after the filter (`0x51a4ba`:
  `w*h*2 ≤ budget / (2 + tripplebuffer)`).
- Remove the 4:3 test in the options list and in `f_0051A530` (byte patch on the branch, or a
  hook for `f_0051A530`), so wide modes are listed and selectable. "Nearest width" in the lookup
  must still pick the saved mode exactly when width alone is ambiguous (1920x1080 vs 1920x1200
  vs 1920x1440): compare height too.
- `RT_SCREEN` (page passes the screen pixels) keeps hiding modes bigger than the screen.
- The game's mode list text (Options › Graphics › Screen Size) must fit the wider labels.
- Check: pick 1280x720 in Options natively, start a race: it loads and runs (the NFS3 comment
  in glide.c says NFS3 never finished loading a race wider than 16:9; find out NFS4's limit),
  the picture fills 16:9 (stretched until P3), menus still 640x480.

**Done when**: wide modes listed, saved, restored after restart. **Findings** (2026-11-27):

- Hooks `mode_list` (`0x51a390`) and `mode_lookup` (`0x51a530`) in `runtime/widescreen.c`, no
  byte patches. The 16 records (in Options order): 4:3 640x480, 800x600, 1024x768, 1280x960,
  1600x1200, 1920x1440, 2880x2160; 16:9 1280x720, 1366x768, 1600x900, 1920x1080, 2560x1440,
  3840x2160; 16:10 1440x900, 1920x1200, 2560x1600. Dropped 4:3: 960x720, 1152x864, 1400x1050,
  1440x1080, 1792x1344, 2048x1536, 2560x1920 (old configs get the nearest 4:3: 1152 → 1024x768).
- **The record count stays 16**: the records are a fixed array in the driver followed by the
  record → GR_RESOLUTION code map, which the driver indexes by record. The Options list holds
  at most 20 entries (`cmp [0x727524], 0x14` in the callback).
- **The height**: only the width is saved (`0x7c3d24`, read only at `0x4c2bb0` for the lookup;
  `0x7c3d20` is the entry index, which shifts when RT_SCREEN changes the list). The list's
  entries are 276 bytes (not 260), `{mode number, w, h, depth, buffers ok, "%d x %d x %d (z)"}`;
  for a non-4:3 mode the hook stores `w | h << 16` as the entry's width, which the menu copies
  to `0x7c3d24` and config.dat. Lookup: exact size, else nearest width of the same aspect, else
  4:3, else any. Checked: plain 1280 → 1280x960; 1152 → 1024x768; 2560x1440 with
  RT_SCREEN=1920x1080 → 1920x1080; 3840x2160 with Triple Buffering → 3840x2160. A production
  build reading a staging config with a wide mode would take the encoded value as a huge
  width (its nearest 4:3 = the biggest mode that fits): harmless, and staging's OPFS is
  separate anyway (other origin).
- The game's video-memory check in the list (`w·h·2 ≤ [0x5befa4]/(2+tb)`, `0x5befa4` read
  nowhere else) is dropped: with it 3840x2160 (and 2880x2160 with Triple Buffering) never showed.
- Menu test natively (Options › Graphics › Screen Size, the drop-down lists all 16 with a scroll
  bar; labels fit): picked 1280 x 720, Done, Exit Game → config.dat width `0x2d00500`, index 3;
  restarted: lookup → 1280x720, the race opens at 1280x720. New players still start at 640x480.
- Races load and run at 1280x720, 3840x2160 (Triple Buffering, 64 fps natively) and **21:9**
  (2560x1080, tested in a temporary slot): NFS4 has no 16:9 limit like NFS3. Picture: square
  pixels, Vert− (P3), HUD stretched sideways (P5), the tachometer needle overshoots its dial at
  21:9. A 21:9 slot would cost one of the 16 (1366x768 or 1440x900): decide after P5.
- With Wide Screen on at 16:9 the 2:1 band leaves 40-pixel bars (1280x720: view y −20, h 720).

## P3: Hor+

- Hook `f_00453F10`: in a wide viewport compute the focal length from `h · 4/3` instead of `w`,
  so the vertical FOV matches 4:3 at the same height and the horizontal one widens. 4:3 views
  stay bit-identical. Cover all its callers (main view, split-screen views, the `0x853a70` view,
  whatever the mirror uses) and treat each view by its own aspect, not the screen's.
- Find any other place that assumes the 4:3 aspect: the second read of the half sizes at
  `0x53a043`/`0x53a06f`, fixed FOV tables per camera (bumper, hood, chase, far chase, the TV
  cameras in replays), and the "fov" field the cameras pass.
- Check: frames at 1280x960 and 1280x720 from the same spot: the 720 frame shows the same
  vertical content and more at the sides, no stretching (round wheels, round signs).

**Done when**: every camera is Hor+. **Findings** (2026-11-27):

- `hook_view_setup` scales the focal lengths of a full-screen view (projection w × h = the main
  record `0x853a10`'s, i.e. the screen: main view, both split-screen halves, the intro's views,
  `0x539f30`'s fov-120 view) by `min(1, ref·h/w)`, ref 4/3, or 2 with Wide Screen on (not in
  split screen). 4:3 screens: untouched. `hook_camera_view` (`0x539f80`) gives a camera's zoom
  path the same factor on x.
- **FOV** comes from `f_00539E10`: 90°, 75° with View Angle (`0x7c3d50` = 1), +15° with Wide
  Screen, halved (45/37/52/45); `n` = fov·13/16. The intro has a fov-40 view. The Hor+ factor is
  relative, so View Angle keeps working.
- **Wide Screen at 16:9** keeps the original: its band is w × w/2, so it's the same 2:1 picture
  as at 4:3, now with 40-pixel bars at 1280x720. (Wider than 2:1 it would turn Hor+ against 2:1;
  no such mode now.) Decided this way instead of a Hor+ letterbox (a 2.67:1 strip at 16:9).
- Checked natively (frames in `build/baseline/p3-*`): the grid at the countdown in the cockpit at
  1280x960 and 1280x720: the same tunnel roof and steering wheel heights, more at the sides
  (A-pillar, door mirror); chase camera at 16:9; Wide Screen on at 4:3 and 16:9: the same band
  picture; split screen at 1280x720 (Split Screen Arcade: the name dialog twice, Ok at 290,294):
  both halves Hor+, race runs.
- **Not covered here**: the mirror (`0x853a70`, projection = the HUD's mirror rect × h/4: its
  shape at 16:9 follows the HUD's scaling: P5/P6); the zoom path didn't run in any race camera
  or the intro (log `ws: camera_view zoom` with RT_WS_LOG=1): likely the replays' TV cameras, to
  check in P6. Camera type 8 (`0x43a650`) restores focal and centre from a copy at `0x660fa8`
  whose writer I didn't find (no direct stores): check in P6 whether it ever shows.
- Readers of the focal lengths (projection, probably culling: 26 places) follow by themselves;
  the half sizes are read only by the setup and the zoom path. P4 starts from those readers.
- 2D overlays drawn in the 3D pass are stretched at 16:9: the start lights, the HUD (P5).

## P4: culling, clipping, effects

With a wider view, things culled for a 4:3 frustum pop in at the edges. Find and widen:

- the frustum or side planes used for object and track-block visibility (look for code that
  reads `0x5b0950`–`0x5b095c` or `0x680f98`, and for the track's per-block visibility lists:
  NFS4 tracks draw neighbouring blocks from precomputed lists, which may be too short for a
  wider view; if so, draw a few more blocks rather than editing data);
- the 2D clip and guard bands (`grClipWindow` from the viewport rect: should follow by itself);
- screen-space effects placed with 640- or 4:3-based numbers: sun and lens flare, track glows,
  headlight and brake-light glows, smoke, sparks, rain and snow overlays, the sky and horizon.
- Check: drive the edges of a few tracks (Celtic Ruins, Hometown, Empire City), compare with 4:3
  frames for pop-in; sunset/sunrise tracks for the flare.

**Done when**: nothing pops at the sides on three tracks. **Findings** (2026-11-28):

- **Nothing to change.** Projection and screen clipping (`f_00453FB0`: outcodes against the
  viewport rect `0x680f78`–`0x680f94`) follow the focal length and the screen, and so does the
  culling: Celtic Ruins at 2560x1080 (21:9, ±60° across) and at 4:3 with only focal x × 0.35
  (±71° across, a temporary hack, removed) has walls, ground, trees and cars right alongside
  drawn to the frame edges. (Scaling both focals widens the vertical view too, and then the
  ground under and behind the camera is missing at the bottom: irrelevant, Hor+ keeps 4:3's
  vertical view.)
- Checked at 1280x720, autopilot, chase camera (contact sheets in `build/baseline/p4-*`): Celtic
  Ruins, Germany (town buildings to the edges), Coastal (lighthouse beam, sparks), Snowy with
  weather (snow over the whole width), night (tail-light glows round, the moon). The sun is a
  projected sprite: round and the same size relative to the height as at 4:3; no separate flare
  elements showed in either.
- **Hometown and Empire City are locked** in a fresh savedata (track `+0x10e0` = 10 or 18 races
  Celtic Ruins), so Germany and Coastal stood in. Track indices (`0x5ba21c`): 0 Hills (Celtic
  Ruins), 1 Germany, 2 Coastal, 3 Park, 4 France, 5 Uk, 6 Snowy, 7–9 Gt1–3, 10 Hometown,
  11 Redrock, 12 Atlantic, 13 RockyPas, 14 Country, 15 LostCany, 16 Aquatica, 17 Summit, 18 Empire.
- Test configs: `build/configs/1280x720-nolb-{germany,coastal,snowy-weather,night}.dat` (night
  `+0x10f4`, weather `+0x10f8`). Contact sheets were made with a small scratch montage script
  (nearest-neighbour downscale, tiled PPM); add it to tools/ if P5 needs more of them.

## P5: HUD and 2D overlays

The HUD is laid out for 640x480. Find how it is placed (a scale of `w/640`, `h/480` somewhere,
or the 2D draw routines taking 640x480 coordinates) and change the horizontal scale to
`h/480` with an offset, so it keeps its proportions:

- anchored groups: tachometer/speedo and gear to the right edge, the map and position to the
  left, lap and time top centre or wherever the game puts them, countdown and messages centred;
- full-screen 2D (loading screens, the pause menu, the race-end and results screens, fades)
  pillarboxed at 4:3 in the middle, with black or the scene at the sides, as looks right;
- an option to keep the HUD inside the 4:3 centre (some players prefer it) if it's cheap.

Prefer doing this in the game's own 2D code (hooks) over guessing in the renderer which
triangles are HUD. **Done when**: HUD looks right at 16:9 and 16:10 and unchanged at 4:3. **Findings**:

## P6: the rest of the race screen

- Rear-view mirror: its own view; keep its shape and place (top centre), Hor+ inside it.
- Split screen (two players, `[0x7c4bec] == 1`): two views of w × h/2; each is Hor+ by its own aspect.
- Replays (Load › Replays, and after a race): TV cameras, the replay bar.
- Hot Pursuit: the cop radar and the arrest screens. High Stakes and career: their race-end screens.
- Online: the in-race chat or messages, if any.

**Done when**: each checked natively once. **Findings**:

## P7: page and settings

- `runtime/gpu.c` letterboxes by the frame's own size, and `web/main.js` (mouse, ~line 902) maps
  by it, so wide frames should already fill a wide screen; check phones (landscape) and an
  ultrawide window (pillarboxed 16:9 if 21:9 is not offered).
- New players: the game starts in the biggest mode that fits (`RT_SCREEN`); on a 16:9 screen
  that would now be a wide mode. Decide with the user whether that's the default, or 4:3 stays
  the default with wide modes in Options. Old config.dat files keep their mode.
- If a wide mode turns out to break something we can't fix (a screen, a mode), leave that mode
  out instead of shipping a broken one.

**Done when**: staging tested by the user on desktop and phone. **Findings**:

## P8: frame rate and pacing

The Vulkan Thrash driver's "uncapped fps" claim. NFS4 runs its timer at 128 Hz and races cap at
64 fps (README). Find out first, then decide:

- where the cap is (the frame loop waits for the tick counter?) and whether the simulation step
  is fixed (1/64 s ticks, laps count in 1/64 s) or uses the frame time;
- if the simulation is fixed-step, more frames only repeat the same picture unless car and camera
  positions are interpolated between steps: likely too much for this patch; write down what
  it would take and stop there;
- what does matter in a browser: even frame pacing at 60, 120 and 144 Hz displays (frames
  presented on a steady cadence, no 64-on-60 judder). Measure with the page's frame stats.

Any change here must leave the race clock and lap times exactly as they are (lap records!).
**Done when**: findings written; pacing fixed if it was off. **Findings**:

## P9: the mods

The user downloads them into `build/mods/` (links in the GitHub issue; HS Music And Tweaks'
archive password is in the issue). Each archive is unpacked into its own new, empty folder.
Never run them under Windows or Wine; Python on them only with `-I`.

1. **List what each changes.** Write `tools/moddiff.py`: for a patched exe, a byte diff against
   ours grouped into ranges, each range disassembled and named by the lifted function it falls
   in; for a hook DLL (`dinput.dll` proxy or `.asi`), its imports and exports, the addresses it
   writes into the exe (search its code and data for `0x4xxxxx`/`0x5xxxxx` constants and patch
   tables), and the strings that name features or settings. Their readme and ini files say what
   each option is for.
2. **Sort every change** into: already done by our runtime (the multicore and affinity fixes:
   we run one core on purpose, ARCHITECTURE.md "Single-core mode"), not needed in a browser
   (the dinput proxy, Windows fixes), done in P2–P8 (widescreen, frame cap), and new.
3. **Implement the new ones in our code** where they make sense:
   - *Faster loading*: our loader is paced by the 128 Hz service thread, one ~8 KB read per
     7.8 ms tick (ARCHITECTURE.md). If the mod raises the chunk size or removes waits, do the
     same with a hook or data change, and measure menu and race load times natively before and
     after. Check it doesn't reorder streaming (the chunk release race noted in ARCHITECTURE.md).
   - *Interactive music from OGG files*: find how the mod switches tracks with the race state
     (the game's own interactive music engine and its `.mus`/`.map` files). Playing the player's
     own OGG files would need decoding (Web Audio `decodeAudioData` on the page, or a small
     decoder in the runtime) and the files coming from the player's game folder (the page
     already imports the folder into OPFS). The music itself is EA's soundtrack: never in our
     pack or repo. Only do this if the game's own music path can't simply get better quality
     from the files the player already has.
   - Anything else (skipped logos, options): case by case.
4. Credit the authors in README.md for what we took ideas from. Ask the user before shipping
   anything that copies their work rather than reimplementing it.

**Done when**: a table of every mod change with its decision, and the chosen ones done. **Findings**:

## P10: Positional Updates 8 → 32 Hz (optional)

From the same GitHub issue. Positional Updates send about 8 position packets/s (~240 bytes);
Controller Updates already send 32/s.

- Find the send rate (a tick divider or interval in the net code; trace with `RT_NET_TRACE`).
- 32/s × 240 B ≈ 7.7 KB/s per peer each way: fine for WebRTC and the relay, but check the
  relay's limits and the room log after a test.
- **Compatibility**: a different rate between two builds may or may not break a race. Test an
  old build against the new one with `tools/nettest.sh`. If they don't mix, the online build
  check must keep them apart, or ship it only once everyone has the new build.
- Measure the gain: car positions of the remote player, smoother at 150 ms delay
  (`RT_NET_DELAY=75` both sides).

**Done when**: measured and decided with the user. **Findings**:

## P11: release

- README.md (Graphics modes: no more "4:3, pillarboxed"; status line), ARCHITECTURE.md (hooks,
  Hor+, HUD, any mod-derived changes), `web/changelog.json` entry.
- The user tests on staging.nfs4.xyz with others (password from `tools/staging.sh password`).
- Commits split by topic when the user asks; merging into main and deploying to nfs4.xyz only
  when the user says so. After the merge, production needs `RELIFT=1 ./build.sh game` and
  `./build-web.sh game` (the hook list changes the lifted code).

## Testing notes

- `tools/race.sh <dir> <seconds>` drives a single race by clicks; `EXTRA=` adds more input.
- Screen mode for a test: a config.dat with the mode's width (`0x7c3d24`) and depth; or
  `RT_GLIDE_MODES=<codes, hex>` to offer only some modes.
- Frames are PPM in the dump folder; `tools/ppm2png.py` converts them for viewing.
- `RT_CURSOR_ADDR=689510` for clicks in menus (the game moves its cursor).
- Restore savedata after every native test (CLAUDE.md).
