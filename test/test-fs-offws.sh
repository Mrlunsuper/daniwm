#!/bin/bash
# test-fs-offws.sh - audit-0930 #5: setfullscreen on an off-workspace
# window must not focus it (no active flip, no BadMatch focus error).
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
OUT=$(python3 "$TDIR/fs-offws.py" 2>&1)
echo "$OUT"
fail=0
echo "$OUT" | grep -q '^PASS' || fail=1
if grep -q BadMatch "$H/wm.err"; then echo "FAIL: BadMatch in wm stderr"; fail=1
else echo "PASS: no BadMatch"; fi
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
