#!/bin/sh
# Browser build: recompiled game + runtime -> web/dist/nfs4.{js,wasm}
#   ./build-web.sh <game dir>
# Needs Emscripten (emcc) on PATH, or deps/emsdk activated.
# web/dist is derived from the player's own nfs4.exe: never commit or publish it.
set -e
cd "$(dirname "$0")"
GAME="${1:-game}"
[ -f deps/emsdk/emsdk_env.sh ] && . deps/emsdk/emsdk_env.sh >/dev/null 2>&1
[ -x /usr/lib/emscripten/emcc ] && PATH="/usr/lib/emscripten:$PATH"   # Arch/CachyOS package
command -v emcc >/dev/null || { echo "emcc not found: install Emscripten (see README / build notes)"; exit 1; }
[ -f gen/modules.c ] || ./build.sh "$GAME"          # lifts the modules (shared with the native build)
mkdir -p build/wasm-obj web/dist
CFLAGS="-O3 -w -fno-strict-aliasing -pthread -Iruntime -DRT_NO_SDL"
ls gen/*/*.c gen/modules.c runtime/*.c | xargs -P"$(nproc)" -I{} sh -c '
  d=$(dirname {}); inc=""; [ "$d" != runtime ] && inc="-I$d"
  o=build/wasm-obj/$(echo {} | tr / _ | sed "s/\.c$/.o/")
  if [ ! -f "$o" ] || [ {} -nt "$o" ] || [ runtime/cpu.h -nt "$o" ] || [ runtime/rt.h -nt "$o" ]; then emcc '"$CFLAGS"' $inc -c {} -o "$o" || exit 255; fi'
emcc build/wasm-obj/*.o -o web/dist/nfs4.js -O3 -pthread \
  -sWASMFS -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=20 \
  -sINITIAL_MEMORY=1024MB -sSTACK_SIZE=64MB -sDEFAULT_PTHREAD_STACK_SIZE=8MB \
  -sMODULARIZE -sEXPORT_ES6 -sEXPORT_NAME=NFS4 -sENVIRONMENT=web,worker \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_rt_gpu_frame,_rt_gpu_set_max_scale,_rt_gpu_stats,_rt_web_input,_rt_web_audio,_rt_web_key_index,_rt_net_packet,_rt_net_set_node,_rt_net_worker -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
  -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8,HEAPF64,UTF8ToString -sEXIT_RUNTIME=0
SHA=$(sha256sum "$GAME/nfs4.exe" | cut -d' ' -f1)
printf '{"exe_sha256":"%s","built":"%s"}\n' "$SHA" "$(date -u +%FT%TZ)" > web/dist/build-info.json
ls -la web/dist
echo "serve with: node web/serve.mjs   then open http://localhost:8080/"
