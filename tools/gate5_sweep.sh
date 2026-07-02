#!/usr/bin/env bash
# Gate-5 fixture sweep: for each fixture ROM, regenerate + build the recomp
# artifact (game only, no SameBoy oracle), run to a frame limit, and dump the
# final screenshot so its own pass/fail verdict can be read. Optionally run the
# A-vs-B pairing to pin a chain.
#
# Usage: gate5_sweep.sh <frames> <name>=<rom_path> [<name>=<rom_path> ...]
set -u
export CLEAN_PATH="/c/msys64/mingw64/bin:/usr/bin:/bin"
WT="F:/Projects/gbcrecomp/_wt-cosim"
GBRECOMP="$WT/build/bin/gbrecomp.exe"
PY=/c/msys64/mingw64/bin/python3.exe
cd "$WT" || exit 1
mkdir -p logs/gate5

FRAMES="$1"; shift
DUMP=$((FRAMES - 10))

for pair in "$@"; do
  name="${pair%%=*}"
  rom="${pair#*=}"
  outdir="${name}_test"
  echo "======================================================================"
  echo "=== $name  ($rom)"
  if [ ! -f "$rom" ]; then echo "  !! ROM missing, skip"; continue; fi
  PATH="$CLEAN_PATH" "$GBRECOMP" "$rom" -o "$outdir" >/dev/null 2>&1 || { echo "  !! regen failed"; continue; }
  PATH="$CLEAN_PATH" cmake -G Ninja -S "$outdir" -B "$outdir/build" \
    -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
    -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe >/dev/null 2>&1
  PATH="$CLEAN_PATH" ninja -C "$outdir/build" >/dev/null 2>&1 || { echo "  !! build failed"; continue; }
  exe="$outdir/build/${name}.exe"
  [ -f "$exe" ] || exe=$(ls "$outdir/build/"*.exe 2>/dev/null | head -1)
  printf '%s\n' "$rom" > "$(dirname "$exe")/rom.cfg"
  PATH="$CLEAN_PATH" "$exe" --limit-frames "$FRAMES" --dump-frames "$DUMP" \
    --screenshot-prefix "logs/gate5/${name}" >/dev/null 2>&1
  ppm=$(ls "logs/gate5/${name}"_*.ppm 2>/dev/null | tail -1)
  if [ -n "$ppm" ]; then
    PATH="$CLEAN_PATH" "$PY" -c "from PIL import Image; Image.open('$ppm').convert('RGB').save('${ppm%.ppm}.png')" 2>/dev/null
    echo "  screenshot: ${ppm%.ppm}.png"
  else
    echo "  !! no screenshot produced"
  fi
done
echo "======================================================================"
echo "DONE"
