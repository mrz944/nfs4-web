#!/bin/sh
# Native (Linux) build of the recompiled game, for bring-up and debugging.
#   ./build.sh <game dir>        lift (if needed) + compile
#   RELIFT=1 ./build.sh <dir>    force re-lift
# Lifted modules: nfs4.exe, EA sound (eacsnd.dll) and the Glide3 thrash driver (drivers/glide3x/voodoo2a.dll).
set -e
cd "$(dirname "$0")"
GAME="${1:-game}"
SEEDS=""; [ -f build/missing.txt ] && SEEDS="--seeds build/missing.txt"
# name, pe, base: every module gets a distinct base so lifted addresses never collide
MODULES="nfs4:nfs4.exe:400000 eacsnd:eacsnd.dll:b00000 voodoo2a:drivers/glide3x/voodoo2a.dll:c00000"
for m in $MODULES; do
  n=${m%%:*}; r=${m#*:}; pe=${r%%:*}; base=${r##*:}
  if [ ! -f "gen/$n/dispatch.c" ] || [ -n "$RELIFT" ]; then
    rm -rf "gen/$n"; .venv/bin/python -I tools/lift.py "$GAME/$pe" "gen/$n" $SEEDS --name "$n" --base "$base"
  fi
done
{
  echo '#include <stddef.h>'; echo '#include <stdint.h>'
  echo 'typedef struct { uint32_t addr; void (*fn)(void *); } FuncEnt;'
  for m in $MODULES; do n=${m%%:*}; echo "extern const FuncEnt g_functab_$n[]; extern const size_t g_functab_${n}_len;"; done
  echo 'const struct { const char *name; const FuncEnt *tab; const size_t *len; uint32_t base; } g_lifted[] = {'
  for m in $MODULES; do n=${m%%:*}; r=${m#*:}; pe=${r%%:*}; base=${r##*:}
    echo "  {\"$(basename "$pe")\", g_functab_$n, &g_functab_${n}_len, 0x${base}u},"; done
  echo '  {0}};'
} > gen/modules.c
mkdir -p build/obj
CFLAGS="-O2 -g -w -fno-strict-aliasing -Iruntime $(pkg-config --cflags sdl2 2>/dev/null)"
ls gen/*/*.c gen/modules.c runtime/*.c | xargs -P"$(nproc)" -I{} sh -c '
  d=$(dirname {}); inc=""; [ "$d" != runtime ] && inc="-I$d"
  o=build/obj/$(echo {} | tr / _ | sed "s/\.c$/.o/")
  if [ ! -f "$o" ] || [ {} -nt "$o" ] || [ runtime/cpu.h -nt "$o" ] || [ runtime/rt.h -nt "$o" ]; then clang '"$CFLAGS"' $inc -c {} -o "$o" || exit 255; fi'
clang build/obj/*.o -o build/nfs4 -lm -lpthread $(pkg-config --libs sdl2) -lGLESv2 -lEGL
echo "built build/nfs4"
