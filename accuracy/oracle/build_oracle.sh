#!/usr/bin/env bash
# Build the SameBoy-backed GB audio oracle with mingw64 gcc.
# Compiles SameBoy's portable Core/*.c directly (the upstream Makefile is
# clang/--target-only on Windows). Requires mingw64/bin on PATH so cc1.exe
# can load its DLLs (see reference_mingw_path_gotcha memory).
set -euo pipefail
export PATH="/c/msys64/mingw64/bin:$PATH"

SAMEBOY="${SAMEBOY:-/f/Projects/SameBoy}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OBJ="$HERE/obj"
OUT="$HERE/gb_audio_oracle.exe"
mkdir -p "$OBJ"

CFLAGS=(-std=gnu11 -O2 -g
        -D_GNU_SOURCE
        -DGB_INTERNAL -D_USE_MATH_DEFINES
        -DGB_VERSION='"oracle"' -DGB_COPYRIGHT_YEAR='"2026"'
        -Drandom=rand
        -Wno-unused-result -Wno-multichar -Wno-format
        -I"$SAMEBOY" -I"$SAMEBOY/Core" -I"$SAMEBOY/Windows")

# Core translation units. Exclude nothing — the debugger stays in so struct
# layout matches the upstream default build.
CORE_SRCS=("$SAMEBOY"/Core/*.c)
# Windows POSIX shims SameBoy ships (getline, vasprintf, etc.). crt.c is CRT
# startup glue we don't want.
WIN_SRCS=("$SAMEBOY"/Windows/stdio.c "$SAMEBOY"/Windows/utf8_compat.c
          "$SAMEBOY"/Windows/math.c "$SAMEBOY"/Windows/dirent.c)

echo "[build] compiling SameBoy Core (${#CORE_SRCS[@]} files) + Windows shims..."
objs=()
for src in "${CORE_SRCS[@]}" "${WIN_SRCS[@]}"; do
    o="$OBJ/win_$(basename "${src%.c}").o"
    [[ "$src" == *"/Core/"* ]] && o="$OBJ/$(basename "${src%.c}").o"
    gcc "${CFLAGS[@]}" -c "$src" -o "$o"
    objs+=("$o")
done

echo "[build] compiling + linking oracle harnesses..."
for harness in gb_audio_oracle gb_state_oracle gb_fb_oracle; do
    gcc "${CFLAGS[@]}" -c "$HERE/$harness.c" -o "$OBJ/$harness.o"
    gcc "${CFLAGS[@]}" "${objs[@]}" "$OBJ/$harness.o" -o "$HERE/$harness.exe" -lm
    echo "[build] OK -> $HERE/$harness.exe"
done
