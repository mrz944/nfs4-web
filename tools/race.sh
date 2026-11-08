#!/bin/sh
# Headless native single race by mouse clicks (start-up smoke test):
#   tools/race.sh <outdir> <seconds>    frames in <outdir>, log <outdir>.log
# Load screen: Cancel → Single Player Arcade → Single Race (320,196) → name dialog Ok → RACE
cd "$(dirname "$0")/.."
D=${1:-build/race}; T=${2:-90}; mkdir -p "$D"
RT_CURSOR_ADDR=689510 RT_MOUSE_SCALE=1 RT_NOSOUND=${RT_NOSOUND-1} RT_HEADLESS=1 RT_DUMP=${DUMP:-60} RT_DUMP_DIR="$D" \
RT_INPUT="9000:CLICK@60;465,11500:CLICK@80;198,14000:MOVE@320;196,14500:CLICK@320;196+300,17000:CLICK@285;278+300,20000:MOVE@335;463,20500:CLICK@335;463+300${EXTRA:+,$EXTRA}" \
  timeout "$T" build/nfs4 game > "$D.log" 2>&1
