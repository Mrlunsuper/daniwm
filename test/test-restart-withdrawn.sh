#!/bin/bash
# test-restart-withdrawn.sh - audit-0930 #4: restart must not re-adopt and
# map a window its client already withdrew.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)
trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"' EXIT INT TERM
WM=0
Xvfb $D -screen 0 1280x800x24 & XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" 2>"$H/wm.err" & WM=$!
sleep 1
kill -0 $WM 2>/dev/null || { echo "FAIL: wm did not start"; exit 1; }
python3 "$TDIR/withdrawn.py" >"$H/out" 2>&1 & PY=$!
sleep 2.5
xdotool key super+ctrl+r
wait $PY
cat "$H/out"
if grep -q '^PASS' "$H/out"; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
