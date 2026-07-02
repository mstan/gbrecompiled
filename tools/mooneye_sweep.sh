#!/usr/bin/env bash
# Mooneye acceptance sweep: build + run each ROM headless, read the final-register
# Fibonacci magic (pass = B C D E H L = 03 05 08 0D 15 22) via GBRT_REGS_LOG.
# Usage: mooneye_sweep.sh <frames> <name>=<rom> [<name>=<rom> ...]
set -u
export CLEAN_PATH="/c/msys64/mingw64/bin:/usr/bin:/bin"
WT="F:/Projects/gbcrecomp/_wt-cosim"
GBRECOMP="$WT/build/bin/gbrecomp.exe"
cd "$WT" || exit 1
mkdir -p logs/mooneye
FRAMES="$1"; shift
pass=0; fail=0; err=0
printf "%-34s %s\n" "TEST" "RESULT"
printf "%-34s %s\n" "----" "------"
for pair in "$@"; do
  name="${pair%%=*}"; rom="${pair#*=}"
  outdir="mn_${name}_test"
  if [ ! -f "$rom" ]; then printf "%-34s %s\n" "$name" "MISSING"; err=$((err+1)); continue; fi
  PATH="$CLEAN_PATH" "$GBRECOMP" "$rom" -o "$outdir" >/dev/null 2>&1 || { printf "%-34s %s\n" "$name" "REGEN-FAIL"; err=$((err+1)); continue; }
  PATH="$CLEAN_PATH" cmake -G Ninja -S "$outdir" -B "$outdir/build" \
    -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
    -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe >/dev/null 2>&1
  PATH="$CLEAN_PATH" ninja -C "$outdir/build" >/dev/null 2>&1 || { printf "%-34s %s\n" "$name" "BUILD-FAIL"; err=$((err+1)); continue; }
  exe=$(ls "$outdir/build/"*.exe 2>/dev/null | head -1)
  printf '%s\n' "$rom" > "$(dirname "$exe")/rom.cfg"
  regs="logs/mooneye/${name}.regs"
  rm -f "$regs"
  GBRT_REGS_LOG="$regs" PATH="$CLEAN_PATH" timeout 25 "$exe" --limit-frames "$FRAMES" >/dev/null 2>&1
  rc=$?
  rm -rf "$outdir"
  if [ $rc -eq 124 ]; then printf "%-34s %s\n" "$name" "HANG"; err=$((err+1)); continue; fi
  if [ ! -f "$regs" ]; then printf "%-34s %s\n" "$name" "NO-DUMP"; err=$((err+1)); continue; fi
  line=$(cat "$regs")
  if echo "$line" | grep -q "B=03 C=05 D=08 E=0D H=15 L=22"; then
    printf "%-34s %s\n" "$name" "PASS"; pass=$((pass+1))
  else
    printf "%-34s %s\n" "$name" "FAIL  [$line]"; fail=$((fail+1))
  fi
  rm -rf "$outdir"
done
echo "----------------------------------------------------"
echo "PASS=$pass  FAIL=$fail  ERR=$err"
