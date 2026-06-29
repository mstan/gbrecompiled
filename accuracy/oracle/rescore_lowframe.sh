#!/usr/bin/env bash
# Fast re-score using ALREADY-BUILT test-rom exes at a low dump frame (for tests
# that render-once-then-halt before frame 20). No rebuild.
set -uo pipefail
MB=/f/Projects/mealybug-tearoom-tests
WT=/f/Projects/gbcrecomp/_wt-accuracy
OUT=$WT/accuracy/out
FRAME="${1:-3}"
TESTS=(m3_bgp_change m3_scx_low_3_bits m3_scy_change m3_lcdc_bg_en_change
       m3_lcdc_obj_en_change m3_lcdc_win_en_change_multiple m3_lcdc_tile_sel_change)
printf "%-34s | %s\n" "m3 test (frame $FRAME)" "recomp vs DMG ground truth"
printf -- "----------------------------------------------------------------\n"
for t in "${TESTS[@]}"; do
    D=$WT/testroms/$t/build
    [ -x "$D/$t.exe" ] || { printf "%-34s | (no exe)\n" "$t"; continue; }
    ( cd "$D" && rm -f rs_*.ppm \
      && timeout 30 bash -c "GBRECOMP_BENCHMARK=1 GBRT_HARDWARE_MODE=dmg ./$t.exe \
           --dump-frames $FRAME --limit-frames $((FRAME+3)) --screenshot-prefix rs >/dev/null 2>&1" )
    ( taskkill //F //IM "$t.exe" >/dev/null 2>&1 ) || true
    rec="$OUT/rs_$t.ppm"; cp "$D/rs_$(printf '%05d' "$FRAME").ppm" "$rec" 2>/dev/null || { printf "%-34s | (recomp no dump @ %d)\n" "$t" "$FRAME"; continue; }
    exp="$OUT/exp_$t.ppm"; magick "$MB/expected/DMG-blob/$t.png" "$exp" >/dev/null 2>&1
    res=$(python "$WT/accuracy/tools/fb_diff.py" "$rec" "$exp" 2>&1 | grep "differing pixels" | sed -E 's/.*differing pixels: //')
    printf "%-34s | %s\n" "$t" "${res:-(diff failed)}"
done
echo "=== m3_obp0_change build error ==="
grep -iE "error|fatal|undefined|abort|assert" "$WT/testroms/m3_obp0_change/build_err.txt" 2>/dev/null | head -5 || echo "(no build_err captured; see recompile)"
