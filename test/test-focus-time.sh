#!/bin/bash
# test-focus-time.sh - audit-0930 #1: stale event timestamp must not make
# the X server ignore later XSetInputFocus (pager activate).
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)
trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"' EXIT INT TERM
WM=0
Xvfb $D -screen 0 1280x800x24 & XVFB=$!
sleep 1
xdotool mousemove 1270 790
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" & WM=$!
sleep 1
kill -0 $WM 2>/dev/null || { echo "FAIL: wm did not start"; exit 1; }
OUT=$(python3 "$TDIR/focus-time.py" 2>&1)
echo "$OUT"
if echo "$OUT" | grep -q '^PASS'; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
