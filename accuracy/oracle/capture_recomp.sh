#!/usr/bin/env bash
# Headless deterministic capture of the recomp's emitted audio stream.
# Runs the prebuilt recomp in benchmark mode (dummy SDL video/audio, no window)
# with --debug-audio; the runtime flushes debug_audio.raw at the emulated-time
# limit. We poll for completion then kill the (otherwise free-running) process.
#
# Usage: capture_recomp.sh <recomp_exe> <seconds> <out.s16>
set -uo pipefail
EXE="$1"; SECS="$2"; OUT="$3"
RUNDIR="$(dirname "$EXE")"
EXP_BYTES=$(( SECS * 44100 * 2 * 2 ))   # stereo S16

cd "$RUNDIR"
rm -f debug_audio.raw
# Model-match the DMG oracle: SGB-enhanced carts (Red/Blue/...) otherwise run the
# recomp's SGB engine (border/palette setup, LCD pulsing) which a DMG oracle skips.
# Override with GBRT_HARDWARE_MODE in the environment to compare another model.
: "${GBRT_HARDWARE_MODE:=dmg}"; export GBRT_HARDWARE_MODE
echo "[capture] launching headless (GBRT_HARDWARE_MODE=$GBRT_HARDWARE_MODE): $(basename "$EXE") --debug-audio --debug-audio-seconds $SECS"
GBRECOMP_BENCHMARK=1 "./$(basename "$EXE")" --debug-audio --debug-audio-seconds "$SECS" \
    > "$RUNDIR/_recomp_capture.log" 2>&1 &
PID=$!

# Poll up to 120s wall for the file to reach the expected size.
for i in $(seq 1 240); do
    if [ -f debug_audio.raw ]; then
        sz=$(stat -c %s debug_audio.raw 2>/dev/null || echo 0)
        if [ "$sz" -ge "$EXP_BYTES" ]; then
            echo "[capture] reached $sz bytes (>= $EXP_BYTES) after ~$((i/2))s wall"
            break
        fi
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "[capture] process exited early"; break
    fi
    sleep 0.5
done

kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
sz=$(stat -c %s debug_audio.raw 2>/dev/null || echo 0)
echo "[capture] final debug_audio.raw = $sz bytes (expected $EXP_BYTES)"
cp -f debug_audio.raw "$OUT"
echo "[capture] copied -> $OUT"
tail -3 "$RUNDIR/_recomp_capture.log" 2>/dev/null || true
