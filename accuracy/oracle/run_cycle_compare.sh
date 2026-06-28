#!/usr/bin/env bash
# Launch the windowed recomp (debug server on :4370), let it free-run to >N frames,
# pull its always-on frame ring, and diff vs the SameBoy state oracle.
# Usage: run_cycle_compare.sh <recomp_exe> <oracle_csv> <frames> <label> <save_recomp_csv>
set -uo pipefail
EXE="$1"; ORACLE="$2"; FRAMES="$3"; LABEL="$4"; SAVE="$5"
TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../tools" && pwd)"
RUNDIR="$(dirname "$EXE")"

cd "$RUNDIR"
# kill any stale instance holding :4370
( taskkill //F //IM "$(basename "$EXE")" >/dev/null 2>&1 ) || true
sleep 1
# Model-match the DMG oracle (override via GBRT_HARDWARE_MODE in the env).
: "${GBRT_HARDWARE_MODE:=dmg}"; export GBRT_HARDWARE_MODE
echo "[run] launching windowed recomp (debug server :4370, GBRT_HARDWARE_MODE=$GBRT_HARDWARE_MODE)"
"./$(basename "$EXE")" > "$RUNDIR/_cyc_run.log" 2>&1 &
PID=$!
# wait for it to render past FRAMES (real-time ~60fps) plus server warmup
WAIT=$(( FRAMES / 55 + 6 ))
echo "[run] waiting ${WAIT}s for ~$FRAMES frames..."
sleep "$WAIT"

echo "[run] pulling ring + comparing"
python "$TOOLS/gb_cycle_compare.py" --oracle "$ORACLE" --port 4370 \
       --frames "$FRAMES" --label "$LABEL" --save-recomp "$SAVE"
rc=$?

kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
( taskkill //F //IM "$(basename "$EXE")" >/dev/null 2>&1 ) || true
exit $rc
