#!/bin/bash
# End-to-end test against a running PipeWire session:
#   play tone -> FxSound sink -> DSP -> target sink, record the target's monitor,
#   compare with the offline DSP reference.
# usage: run_e2e.sh <build-dir> <preset.fac> <target-sink> [extra passthru_test args...]
set -u
B=$1; PRESET=$2; TARGET=$3; shift 3
OUT=${E2E_OUT:-/tmp/fx_e2e}; mkdir -p "$OUT"; rm -f "$OUT/rec.wav"
setsid "$B/passthru_test" "$PRESET" 9 "$TARGET" "$@" </dev/null >"$OUT/harness.log" 2>&1 &
H=$!
sleep 1.5
setsid pw-record --target "$TARGET" -P '{ stream.capture.sink=true }' --format f32 --rate 48000 --channels 2 "$OUT/rec.wav" </dev/null >"$OUT/rec.log" 2>&1 &
sleep 0.5
timeout 10 pw-play "${TONE:-$OUT/tone.wav}" </dev/null
sleep 1; pkill -INT -x pw-record
wait $H
