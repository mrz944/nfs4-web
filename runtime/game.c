/* Lap times for the start page's lap records (NFS4 High Stakes with the Modern Patch: the
 * addresses are its). Found with RT_INPUT MEMDUMP during native races (README, test helpers).
 *
 * Cars: a table of pointers at 0x661a88 (8, then 0) to the car records (0xad4 bytes each, from
 * 0x661d98). In a record:
 *   +0x000  slot (0..7, the racer table's order)
 *   +0x1dc  the car's code, 4 characters as its folder under data/cars ("P911", "FM50", ...)
 *   +0x1ec  which human drives it (0, 1, ...: the same numbers on every machine online), -1 the AI
 *   +0x1f8  5 for a human's car, 11 for the AI's; +0x200 when another machine drives it (online)
 *   +0x22c  laps finished
 *   +0x230  their total, +0x234 the time before the car first crossed the line
 *   +0x238  each finished lap's time, 4 bytes a lap, in 1/64 s (the race clock's ticks)
 *   +0x588  its control routine: 0x460a90 a human's here, 0x4123a0 the AI's, 0x418020 the network's
 * The local player's car is the human car no other machine drives (local_car). The race's
 * settings are below (RACE_*). Each lap the local player finishes goes to the page
 * (Module.onLap), and to the log. Checked natively: offline (all settings), and online with two
 * instances (the host reports car 0's laps, the client car 1's).
 *
 * RT_AUTOPILOT=1 (native tests only): the first human's car is driven by the game's AI (its
 * control routine and kind), so a scripted race finishes with real laps. Its replay has no
 * recording: the human control routine is what records the player's car. */
#include "rt.h"
#include <string.h>
#include <strings.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

#define CAR_TABLE 0x661a88u
#define CAR_SLOT 0x000u
#define CAR_CODE 0x1dcu
#define CAR_HUMAN 0x1ecu
#define CAR_LAPS 0x22cu
#define CAR_LAP_TIMES 0x238u
#define CAR_CONTROL 0x588u
#define CONTROL_HUMAN 0x460a90u
#define CONTROL_AI 0x4123a0u
#define CAR_KIND 0x1f8u                   /* 5 for a human's car, 11 for the AI's; +0x200 driven from another machine */
#define KIND_AI 11u
#define KIND_REMOTE 0x200u
#define MAX_LAPS 16

static uint32_t car_ptr(int k) {
    uint32_t p = RD32(CAR_TABLE + 4u * (uint32_t)k);
    return p >= 0x400000u && p < 0x10000000u ? p : 0;
}

/* the race being driven: filled in by race_info (see there) */
typedef struct { int track, laps, flags; } Race;
extern int game_race(Race *r);

static void lap_report(uint32_t car, int lap, uint32_t ticks) {
    Race r = { -1, 0, 0 };
    game_race(&r);
    uint32_t code = RD32(car + CAR_CODE);
    char cc[5] = {0}; memcpy(cc, &code, 4);
    for (int i = 0; i < 4; i++) if (cc[i] < 32 || cc[i] > 126) cc[i] = '?';
    rt_log("game: lap %d/%d %u:%02u.%02u (track %d, flags %#x, car %s)", lap, r.laps, ticks / 64 / 60, ticks / 64 % 60, ticks % 64 * 100 / 64, r.track, r.flags, cc);
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module.onLap) Module.onLap($0, $1, $2, $3, $4, $5); }, r.track, r.flags, (int)code, lap, r.laps, (int)ticks);
#endif
}

/* the car of this machine's player: a human's car not driven from another machine (online,
 * every machine has every human's car, with the same human numbers); with two on one screen,
 * the first one's */
static uint32_t local_car(void) {
    uint32_t best = 0; int32_t bh = 0x7fffffff;
    for (int k = 0; k < 16; k++) {
        uint32_t p = car_ptr(k);
        if (!p) break;
        int32_t h = (int32_t)RD32(p + CAR_HUMAN);
        if (h < 0 || h > 7 || (RD32(p + CAR_KIND) & KIND_REMOTE)) continue;
        if (h < bh) { bh = h; best = p; }
    }
    return best;
}

static void lap_watch(void) {
    static uint32_t seen_car; static int done;               /* laps of seen_car already reported */
    uint32_t car = local_car();
    if (!car) return;
    int n = (int)RD32(car + CAR_LAPS);
    if (n < 0 || n > 64) return;
    if (car != seen_car) { seen_car = car; done = n; return; }   /* laps from before: not now */
    if (n < done) done = n;                                    /* a new race, or a restart */
    while (done < n && done < MAX_LAPS) {
        uint32_t ticks = RD32(car + CAR_LAP_TIMES + 4u * (uint32_t)done);
        if (!ticks) break;                                     /* written a moment later */
        done++;
        lap_report(car, done, ticks);
    }
}

static void autopilot(void) {
    static int on = -1;
    if (on < 0) { on = getenv("RT_AUTOPILOT") ? atoi(getenv("RT_AUTOPILOT")) : 0; if (on) rt_log("game: autopilot (the AI drives the player's car)"); }
    if (!on) return;
    uint32_t car = local_car();
    if (car && RD32(car + CAR_CONTROL) == CONTROL_HUMAN) {
        WR32(car + CAR_CONTROL, CONTROL_AI);
        WR32(car + CAR_KIND, (RD32(car + CAR_KIND) & ~0xffu) | KIND_AI);    /* the AI's driving */
        rt_log("game: autopilot on car %u", RD32(car + CAR_SLOT));
    }
}

void game_frame(void) {
    autopilot();
    lap_watch();
}

/* The race's settings: in the game's settings block at 0x7c3c04 (the one its code reads; copies
 * of it go into replays and elsewhere), as the Location screen sets them and as the game sets
 * them for its other modes and online races. Track: the index into the track folder table at
 * 0x5ba21c (0 Hills = Celtic Ruins, 1 Germany, ... 18 Empire). */
#define RACE_TRACK 0x7c4ce4u
#define RACE_LAPS 0x7c4cecu
#define RACE_BACKWARD 0x7c4cf0u
#define RACE_MIRRORED 0x7c4cf4u
#define RACE_NIGHT 0x7c4cf8u
#define RACE_WEATHER 0x7c4cfcu
int game_race(Race *r) {
    r->track = (int)RD32(RACE_TRACK);
    r->laps = (int)RD32(RACE_LAPS);
    r->flags = (RD32(RACE_BACKWARD) ? 1 : 0) | (RD32(RACE_MIRRORED) ? 2 : 0) | (RD32(RACE_NIGHT) ? 4 : 0) | (RD32(RACE_WEATHER) ? 8 : 0);
    return r->track >= 0 && r->track < 19;
}
