#!/usr/bin/env bash
# Recompile + build + run a gbdev test ROM through the recomp, dump frame N, and
# diff vs a reference (SameBoy gb_fb_oracle and/or a Mealybug expected PNG).
# The recomp is a per-ROM static recompiler, so each test ROM is recompiled.
#
# Usage: run_testrom.sh <rom.gb> <frame> [expected.png]
set -uo pipefail
ROM="$1"; FRAME="$2"; EXPECTED="${3:-}"
# Do NOT prepend mingw globally: it shadows the pyenv python (numpy). The default
# PATH already has mingw64/bin (for exe DLLs); cmake/ninja use CLEAN_PATH explicitly.
export CLEAN_PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:/c/Windows/system32:/c/Windows"

REPO=/f/Projects/gbcrecomp/gb-recompiled
WT=/f/Projects/gbcrecomp/_wt-accuracy
RT="F:/Projects/gbcrecomp/_wt-accuracy/runtime"
OUT="$WT/accuracy/out"
name=$(basename "${ROM%.*}")
D="$WT/testroms/$name"
romwin=$(cygpath -w "$ROM" 2>/dev/null | sed 's#\\#/#g'); romwin="${romwin:-$ROM}"

echo "[testrom] recompiling $name ..."
"$REPO/build/bin/gbrecomp.exe" "$ROM" -o "$D" >/dev/null 2>&1
# Point the generated project's runtime at the worktree runtime (absolute).
sed -i -E "s#set\\(GBRT_DIR \"[^\"]*\"\\)#set(GBRT_DIR \"$RT\")#" "$D/CMakeLists.txt"
echo "[testrom] building ..."
PATH="$CLEAN_PATH" cmake -G Ninja -S "$D" -B "$D/build" \
    -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
    -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe >/dev/null 2>&1
PATH="$CLEAN_PATH" ninja -C "$D/build" >/dev/null 2>&1 || { echo "[testrom] build FAILED"; exit 1; }

# Pre-fill rom.cfg so the launcher loads the exact ROM (SHA-checked) headless.
echo "$romwin" > "$D/build/rom.cfg"
echo "[testrom] running recomp -> frame $FRAME"
# --limit-frames makes benchmark mode exit after dumping (it otherwise loops
# forever); timeout is a hard safety net.
( cd "$D/build" && rm -f tr_*.ppm \
  && timeout 40 bash -c "GBRECOMP_BENCHMARK=1 GBRT_HARDWARE_MODE=dmg ./$name.exe \
       --dump-frames $FRAME --limit-frames $((FRAME + 5)) --screenshot-prefix tr >/dev/null 2>&1" )
( taskkill //F //IM "$name.exe" >/dev/null 2>&1 ) || true
rec="$OUT/recomp_tr_$name.ppm"
cp "$D/build/tr_$(printf '%05d' "$FRAME").ppm" "$rec"

# SameBoy oracle (note: blank on some Mealybug ROMs -- prefer the expected PNG).
clean="$OUT/${name}.gb"; cp "$ROM" "$clean"
ora="$OUT/oracle_tr_$name.ppm"
timeout 30 "$WT/accuracy/oracle/gb_fb_oracle.exe" "$clean" "$ora" "$FRAME" dmg >/dev/null 2>&1 || true

echo "[testrom] === vs SameBoy oracle ==="
python "$WT/accuracy/tools/fb_diff.py" "$rec" "$ora" 2>&1 | grep -E "differing|MISMATCH|levels" || true
if [ -n "$EXPECTED" ]; then
    exp="$OUT/expected_tr_$name.ppm"
    magick "$(cygpath -w "$EXPECTED" 2>/dev/null || echo "$EXPECTED")" "$exp" >/dev/null 2>&1
    echo "[testrom] === vs Mealybug expected (ground truth) ==="
    python "$WT/accuracy/tools/fb_diff.py" "$rec" "$exp" 2>&1 | grep -E "differing|MISMATCH|levels" || true
fi
