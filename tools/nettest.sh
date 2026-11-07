#!/bin/sh
# Two headless native instances in one relay room (dev test for online play).
#   tools/nettest.sh <relay unix socket> <seconds> "<host RT_INPUT>" "<client RT_INPUT>"
# Frames: build/net/{a,b}/gNNNNNN.ppm, logs: build/net/{a,b}.log
cd "$(dirname "$0")/.."
SOCK=$1; T=$2
rm -rf build/net && mkdir -p build/net/a build/net/b
common="RT_CURSOR_ADDR=689510 RT_MOUSE_SCALE=1 RT_HEADLESS=1 RT_NOSOUND=1 RT_DUMP=${DUMP:-30} RT_NET=unix:$SOCK RT_ROOM=${ROOM:-nettest} RT_NET_STATS=1 RT_NET_TRACE=1"
env $common RT_NAME=host RT_DUMP_DIR=build/net/a RT_INPUT="$3" timeout "$T" build/nfs4 game > build/net/a.log 2>&1 &
sleep 0.5
env $common RT_NAME=client RT_DUMP_DIR=build/net/b RT_INPUT="$4" timeout "$T" build/nfs4 game > build/net/b.log 2>&1 &
wait
