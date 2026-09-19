#!/bin/bash
# test-remap-steal.sh - T-H4: known remap never steals sel/active.
# New manages still focus. Covers tile and monocle (visible stays put).
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

fail=0
assert() { local desc=$1; shift; if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi; }

cleanup() {
    kill $WM $XVFB 2>/dev/null
    rm -rf "$H"
}
trap 'cleanup; exit $fail' EXIT INT TERM
WM=0; XVFB=0

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
kill -0 $WM 2>/dev/null || { echo "FAIL: wm did not start"; exit 1; }

echo "--- tile ---"
OUT=$(python3 "$TDIR/remap-steal.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
TILE_OK=$(echo "$OUT" | sed -n 's/^REMAP_OK=//p' | tail -n1)
[ -n "${TILE_OK:-}" ] || { echo "FAIL: no REMAP_OK (tile)"; exit 1; }
assert "tile remap never steals sel" "$TILE_OK" -eq 1

echo "--- monocle ---"
xdotool key super+m 2>/dev/null || { echo "FAIL: xdotool super+m"; exit 1; }
sleep 1
OUT=$(python3 "$TDIR/remap-steal.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
MON_OK=$(echo "$OUT" | sed -n 's/^REMAP_OK=//p' | tail -n1)
[ -n "${MON_OK:-}" ] || { echo "FAIL: no REMAP_OK (monocle)"; exit 1; }
assert "monocle remap never steals sel" "$MON_OK" -eq 1
xdotool key super+t 2>/dev/null || true
sleep 0.5

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
