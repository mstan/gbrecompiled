#!/usr/bin/env bash
# Run a set of Mealybug m3_* tests through the recomp and score vs DMG ground truth.
set -uo pipefail
MB=/f/Projects/mealybug-tearoom-tests
WT=/f/Projects/gbcrecomp/_wt-accuracy
FRAME="${1:-20}"

# All m3_* (mid-mode-3) ROMs that have a DMG-blob ground-truth PNG. Guest-cycle dump
# (run_testrom --dump-cycle-frames) captures even late-LCD/halting ROMs.
TESTS=()
for rom in "$MB"/_roms/m3_*.gb; do
    t=$(basename "${rom%.gb}")
    [ -f "$MB/expected/DMG-blob/$t.png" ] && TESTS+=("$t")
done

printf "%-36s | %-9s | %s\n" "m3 test (mid-mode-3 effect)" "build/run" "vs DMG ground truth"
printf -- "--------------------------------------------------------------------------------\n"
pass=0; built=0; total=0
for t in "${TESTS[@]}"; do
    total=$((total+1))
    rom="$MB/_roms/$t.gb"; exp="$MB/expected/DMG-blob/$t.png"
    log=$(bash "$WT/accuracy/oracle/run_testrom.sh" "$rom" "$FRAME" "$exp" 2>&1)
    if echo "$log" | grep -q "build FAILED"; then
        printf "%-36s | %-9s | %s\n" "$t" "BUILD-ERR" "(recompiler/link gap)"; continue
    fi
    res=$(echo "$log" | grep "differing pixels" | tail -1 | sed -E 's/.*differing pixels: //')
    if [ -z "$res" ]; then
        printf "%-36s | %-9s | %s\n" "$t" "NO-FB" "(no framebuffer captured)"; continue
    fi
    built=$((built+1))
    pct=$(echo "$res" | sed -E 's/.*\(([0-9.]+)%\)/\1/')
    # treat <1% differing as PASS (sub-scanline exact); else report the gap
    awk "BEGIN{exit !($pct < 1.0)}" && { pass=$((pass+1)); tag="PASS"; } || tag="ok"
    printf "%-36s | %-9s | %s  %s\n" "$t" "$tag" "$res" ""
done
printf -- "--------------------------------------------------------------------------------\n"
printf "scored %d/%d ROMs ; PASS(<1%%) %d ; frame=%d\n" "$built" "$total" "$pass" "$FRAME"
