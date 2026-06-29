#!/usr/bin/env bash
# Run a set of Mealybug m3_* tests through the recomp and score vs DMG ground truth.
set -uo pipefail
MB=/f/Projects/mealybug-tearoom-tests
WT=/f/Projects/gbcrecomp/_wt-accuracy
FRAME="${1:-20}"
TESTS=(m3_bgp_change m3_obp0_change m3_scx_low_3_bits m3_scy_change
       m3_lcdc_bg_en_change m3_lcdc_obj_en_change m3_lcdc_win_en_change_multiple
       m3_lcdc_tile_sel_change)

printf "%-34s | %-10s | %s\n" "m3 test (mid-mode-3 effect)" "build/run" "vs DMG ground truth"
printf -- "------------------------------------------------------------------------------\n"
for t in "${TESTS[@]}"; do
    rom="$MB/_roms/$t.gb"; exp="$MB/expected/DMG-blob/$t.png"
    log=$(bash "$WT/accuracy/oracle/run_testrom.sh" "$rom" "$FRAME" "$exp" 2>&1)
    if echo "$log" | grep -q "build FAILED"; then
        printf "%-34s | %-10s | %s\n" "$t" "FAIL" "(did not build)"; continue
    fi
    # the last "differing pixels" line is the vs-expected diff
    res=$(echo "$log" | grep "differing pixels" | tail -1 | sed -E 's/.*differing pixels: //')
    printf "%-34s | %-10s | %s\n" "$t" "ok" "${res:-(no output)}"
done
