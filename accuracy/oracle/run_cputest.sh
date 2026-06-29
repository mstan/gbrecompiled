#!/usr/bin/env bash
# Recompile + build + run a CPU/timing test ROM (blargg / mooneye) through the recomp
# and decide pass/fail from its SERIAL output (GBRT_SERIAL_LOG).
#   blargg : prints "...Passed" / "Failed #n" over serial.
#   mooneye: writes the magic Fibonacci 03 05 08 0D 15 22 on success (regs B C D E H L).
# Usage: run_cputest.sh <rom.gb> [frames]
set -uo pipefail
ROM="$1"; FRAMES="${2:-3000}"
REPO=/f/Projects/gbcrecomp/gb-recompiled
WT=/f/Projects/gbcrecomp/_wt-accuracy
RT="F:/Projects/gbcrecomp/_wt-accuracy/runtime"
export CLEAN_PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:/c/Windows/system32:/c/Windows"
name=$(basename "${ROM%.*}" | tr ' ' '_')
D="$WT/testroms/cpu_$name"
romwin=$(cygpath -w "$ROM" 2>/dev/null | sed 's#\\#/#g'); romwin="${romwin:-$ROM}"

( taskkill //F //IM "$name.exe" >/dev/null 2>&1 ) || true
"$REPO/build/bin/gbrecomp.exe" "$ROM" -o "$D" >/dev/null 2>&1 || { echo "$name | RECOMP-ERR"; exit 1; }
sed -i -E "s#set\\(GBRT_DIR \"[^\"]*\"\\)#set(GBRT_DIR \"$RT\")#" "$D/CMakeLists.txt"
PATH="$CLEAN_PATH" cmake -G Ninja -S "$D" -B "$D/build" \
    -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
    -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe >/dev/null 2>&1
PATH="$CLEAN_PATH" ninja -C "$D/build" >/dev/null 2>&1 || { echo "$name | BUILD-ERR"; exit 1; }
exe=$(ls "$D/build/"*.exe 2>/dev/null | head -1)
echo "$romwin" > "$D/build/rom.cfg"
ser="$D/build/serial.txt"; rm -f "$ser"
reg="$D/build/regs.txt"; rm -f "$reg"
( cd "$D/build" && timeout 90 bash -c "PATH=\"/c/msys64/mingw64/bin:\$PATH\" GBRECOMP_BENCHMARK=1 GBRT_HARDWARE_MODE=dmg GBRT_SERIAL_LOG=serial.txt GBRT_REGS_LOG=regs.txt \"$exe\" --limit-frames $FRAMES >/dev/null 2>&1" )
( taskkill //F //IM "$(basename "$exe")" >/dev/null 2>&1 ) || true

# Decide pass/fail.
# mooneye: register magic at completion — PASS = B C D E H L = 03 05 08 0D 15 22
# (Fibonacci); FAIL = all six = 0x42 ('B'). Read via the GBRT_REGS_LOG dump (no
# debugger needed). blargg: "Passed"/"Failed #n" over serial.
verdict="UNKNOWN"
if [ -f "$reg" ] && grep -qE "B=03 C=05 D=08 E=0D H=15 L=22" "$reg"; then
    verdict="PASS(mooneye)"
elif [ -f "$reg" ] && grep -qE "B=42 C=42 D=42 E=42 H=42 L=42" "$reg"; then
    verdict="FAIL(mooneye)"
elif [ -f "$ser" ]; then
    txt=$(tr -d '\0' < "$ser" | tr -c '[:print:]\n' ' ')
    if echo "$txt" | grep -qi "Passed"; then verdict="PASS"
    elif echo "$txt" | grep -qi "Failed"; then verdict="FAIL: $(echo "$txt" | grep -io 'Failed[^ ]* *[#0-9]*' | head -1)"
    else verdict="INCONCLUSIVE (serial: $(echo "$txt" | tr -s ' ' | tail -c 60)$([ -f "$reg" ] && echo "; regs: $(cat "$reg")"))"; fi
else
    verdict="NO-SERIAL$([ -f "$reg" ] && echo " (regs: $(cat "$reg"))")"
fi
printf "%-40s | %s\n" "$name" "$verdict"
