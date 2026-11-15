#!/bin/sh
# Native build that runs the browser's audio path (runtime/dsound.c, RT_WEBAUDIO_SIM): the
# mixer ring and the speaker-paced clock, with a thread in the AudioWorklet's place.
#   tools/audiosim.sh && RT_WAV=out.wav RT_SIM_DRIFT=-0.0003 RT_HEADLESS=1 build/nfs4sim game
# The log says once a minute how the sound went (buffer level, clock, drops, resyncs).
# Needs ./build.sh first (it links the other objects from build/obj).
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/sim
clang -O2 -g -w -fno-strict-aliasing -Iruntime $(pkg-config --cflags sdl2) -DRT_WEBAUDIO_SIM -c runtime/dsound.c -o build/sim/dsound.o &&
clang $(ls build/obj/*.o | grep -v runtime_dsound.o) build/sim/dsound.o -o build/nfs4sim -lm -lpthread $(pkg-config --libs sdl2) -lGLESv2 -lEGL &&
echo "built build/nfs4sim"
