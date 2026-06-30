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
WT=/f/Projects/gbcrecomp/gb-recompiled
RT="F:/Projects/gbcrecomp/gb-recompiled/runtime"
OUT="$WT/accuracy/out"
name=$(basename "${ROM%.*}")
D="$WT/testroms/$name"
romwin=$(cygpath -w "$ROM" 2>/dev/null | sed 's#\\#/#g'); romwin="${romwin:-$ROM}"

echo "[testrom] recompiling $name ..."
# Kill any stale instance first: a still-running exe locks the output file and the
# relink fails with "Permission denied" (looks like a link gap but isn't).
( taskkill //F //IM "$name.exe" >/dev/null 2>&1 ) || true
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
# --dump-cycle-frames triggers on elapsed guest cycles (frame N = N*70224 T-cycles),
# so it captures even when the ROM keeps the LCD off / enables it late / halts after
# rendering (rendered-frame --dump-frames misses those). Benchmark mode exits once the
# cycle target is captured; --limit-frames + timeout are hard safety nets.
# The exe needs mingw64/bin on PATH for its DLLs (SDL2/libstdc++/...). Set it only
# for this launch so the python diff steps still resolve the pyenv python (numpy).
( cd "$D/build" && rm -f tr_*.ppm \
  && timeout 40 bash -c "PATH=\"/c/msys64/mingw64/bin:\$PATH\" GBRECOMP_BENCHMARK=1 GBRT_HARDWARE_MODE=dmg ./$name.exe \
       --dump-cycle-frames $FRAME --limit-frames $((FRAME + 30)) --screenshot-prefix tr >/dev/null 2>&1" )
( taskkill //F //IM "$name.exe" >/dev/null 2>&1 ) || true
rec="$OUT/recomp_tr_$name.ppm"
cp "$D/build/tr_$(printf '%05d' "$FRAME").ppm" "$rec"

# SameBoy oracle. "ldbb" makes it capture at the `LD B,B` software breakpoint
# (how Mealybug signals "screenshot now") with post-boot IO+VRAM state, instead
# of a fixed frame; it falls back to frame N for ROMs that never hit LD B,B.
# Cap at >= 90 frames so a late breakpoint isn't missed.
clean="$OUT/${name}.gb"; cp "$ROM" "$clean"
ora="$OUT/oracle_tr_$name.ppm"
ldbb_cap=$(( FRAME > 90 ? FRAME : 90 ))
timeout 30 "$WT/accuracy/oracle/gb_fb_oracle.exe" "$clean" "$ora" "$ldbb_cap" dmg ldbb >/dev/null 2>&1 || true

echo "[testrom] === vs SameBoy oracle ==="
python "$WT/accuracy/tools/fb_diff.py" "$rec" "$ora" 2>&1 | grep -E "differing|MISMATCH|levels" || true
if [ -n "$EXPECTED" ]; then
    exp="$OUT/expected_tr_$name.ppm"
    magick "$(cygpath -w "$EXPECTED" 2>/dev/null || echo "$EXPECTED")" "$exp" >/dev/null 2>&1
    echo "[testrom] === vs Mealybug expected (ground truth) ==="
    python "$WT/accuracy/tools/fb_diff.py" "$rec" "$exp" 2>&1 | grep -E "differing|MISMATCH|levels" || true
fi
