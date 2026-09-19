#!/bin/bash
# test-direct-state.sh - T-M1: direct _NET_WM_STATE writes converge.
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

OUT=$(python3 "$TDIR/direct-state.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
DOK=$(echo "$OUT" | sed -n 's/^DIRECT_OK=//p' | tail -n1)
COK=$(echo "$OUT" | sed -n 's/^CM_OK=//p' | tail -n1)
[ -n "${DOK:-}" ] || { echo "FAIL: no DIRECT_OK"; exit 1; }
[ -n "${COK:-}" ] || { echo "FAIL: no CM_OK"; exit 1; }

assert "direct write converges fullscreen/tiled without loop" "$DOK" -eq 1
assert "ClientMessage fullscreen path still works" "$COK" -eq 1

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
