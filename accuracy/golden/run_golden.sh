#!/usr/bin/env bash
# Axis 1 golden test: lock the structurally-fragile (HL) RMW / ALU operand
# patterns (magic index 6) and PUSH/POP AF flag masking (magic index 4).
#   Leg 1 (structural): the generated C must read memory at HL for every (HL)
#     op (NOT a bare register — the documented AND (HL) -> gb_and8(ctx,ctx->b)
#     miscompile) and must mask F with 0xFFF0 on PUSH/POP AF.
#   Leg 2 (behavioral): recompile+run; final regs must be B=0F C=3F D=F0 E=F0.
# Exit 0 only if both legs pass.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO=/f/Projects/gbcrecomp/gb-recompiled
RT="F:/Projects/gbcrecomp/_wt-accuracy/runtime"
export CLEAN_PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:/c/Windows/system32:/c/Windows"
ROM="$HERE/golden_hl_af.gb"
GEN="$HERE/gen"
fail=0
note() { printf "  %-9s %s\n" "$1" "$2"; }

python "$HERE/build_golden_rom.py" "$ROM" >/dev/null || { echo "ROM-BUILD-ERR"; exit 1; }
rm -rf "$GEN"
"$REPO/build/bin/gbrecomp.exe" "$ROM" -o "$GEN" >/dev/null 2>&1 || { echo "RECOMP-ERR"; exit 1; }

src="$GEN/golden_hl_af_funcs_0.c"
echo "== Leg 1: structural =="
check() { # <regex> <label>
    if grep -qE "$1" "$src"; then note "PASS" "$2"; else note "FAIL" "$2 (missing: $1)"; fail=1; fi
}
check 'gb_and8\(ctx, gb_read8\(ctx, ctx->hl\)\)'  'AND (HL) reads mem'
check 'gb_or8\(ctx, gb_read8\(ctx, ctx->hl\)\)'   'OR (HL) reads mem'
check 'gb_xor8\(ctx, gb_read8\(ctx, ctx->hl\)\)'  'XOR (HL) reads mem'
check 'gb_add8\(ctx, gb_read8\(ctx, ctx->hl\)\)'  'ADD A,(HL) reads mem'
# RMW (HL): read at HL (bug guard) + cycle-accurate split (read; gb_tick; write).
check 'gb_inc8\(ctx, gb_read8\(ctx, ctx->hl\)\)'  'INC (HL) reads mem at HL'
check 'gb_dec8\(ctx, gb_read8\(ctx, ctx->hl\)\)'  'DEC (HL) reads mem at HL'
check '__rmw = gb_inc8\(ctx, gb_read8\(ctx, ctx->hl\)\); gb_tick\(ctx, 4\); gb_write8\(ctx, ctx->hl, __rmw\)' 'INC (HL) RMW tick-split'
check 'gb_push16\(ctx, ctx->af & 0xFFF0\)'        'PUSH AF masks F'
check 'gb_pop16\(ctx\) & 0xFFF0'                  'POP AF masks F'
# Negative: the documented bug — an (HL) ALU op compiled to a bare register.
if grep -qE 'gb_(and|or|xor|add)8\(ctx, ctx->[bcdehla]\)' "$src"; then
    note "FAIL" "bug pattern present: (HL) op compiled to a bare register"; fail=1
else
    note "PASS" "no bare-register (HL) miscompile"
fi

echo "== Leg 2: behavioral (B=0F C=3F D=F0 E=F0) =="
sed -i -E "s#set\\(GBRT_DIR \"[^\"]*\"\\)#set(GBRT_DIR \"$RT\")#" "$GEN/CMakeLists.txt"
PATH="$CLEAN_PATH" cmake -G Ninja -S "$GEN" -B "$GEN/build" \
    -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
    -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe >/dev/null 2>&1
if PATH="$CLEAN_PATH" ninja -C "$GEN/build" >/dev/null 2>&1; then
    exe=$(ls "$GEN/build/"*.exe 2>/dev/null | head -1)
    echo "$(cygpath -w "$ROM" | sed 's#\\#/#g')" > "$GEN/build/rom.cfg"
    ( cd "$GEN/build" && timeout 60 bash -c "PATH=\"/c/msys64/mingw64/bin:\$PATH\" GBRECOMP_BENCHMARK=1 GBRT_REGS_LOG=regs.txt \"$exe\" --limit-frames 60 >/dev/null 2>&1" )
    ( taskkill //F //IM "$(basename "$exe")" >/dev/null 2>&1 ) || true
    regs="$GEN/build/regs.txt"
    if [ -f "$regs" ] && grep -qE "B=0F C=3F D=F0 E=F0" "$regs"; then
        note "PASS" "$(cat "$regs")"
    else
        note "FAIL" "got: $([ -f "$regs" ] && cat "$regs" || echo '<no regs>')"; fail=1
    fi
else
    note "FAIL" "build error"; fail=1
fi

echo
if [ "$fail" -eq 0 ]; then echo "GOLDEN (HL)/AF: PASS"; else echo "GOLDEN (HL)/AF: FAIL"; fi
exit $fail
