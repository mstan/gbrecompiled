#!/usr/bin/env bash
# Validate the per-opcode access-cycle table (gb_timing.h) is well-formed.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
INC="F:/Projects/gbcrecomp/gb-recompiled/runtime/include"
export CLEAN_PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:/c/Windows/system32:/c/Windows"
PATH="$CLEAN_PATH" gcc -std=c99 -I"$INC" "$HERE/gb_timing_selftest.c" -o "$HERE/gb_timing_selftest.exe"
"$HERE/gb_timing_selftest.exe"
