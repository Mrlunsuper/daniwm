#!/bin/bash
# test-wm-state.sh - T-M4A: WM publishes ICCCM WM_STATE NormalState (1) on
# manage and WithdrawnState (0) on unmanage (synthetic withdraw path).
# WM_STATE is a mirror only; the clients list stays the single source
# of truth. No IconicState yet.
# Manual verification:
#   DISPLAY=$D xprop -id <win> WM_STATE
#   DISPLAY=$D xprop -root _NET_CLIENT_LIST
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

OUT=$(python3 "$TDIR/wm-state.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper setup failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
MS=$(echo "$OUT" | sed -n 's/^MANAGED_STATE=//p' | tail -n1)
UM=$(echo "$OUT" | sed -n 's/^UNMANAGED=//p' | tail -n1)
WS=$(echo "$OUT" | sed -n 's/^WITHDRAWN_STATE=//p' | tail -n1)
[ -n "${MS:-}" ] && [ -n "${UM:-}" ] && [ -n "${WS:-}" ] || { echo "FAIL: incomplete helper output"; exit 1; }

assert "WM_STATE is NormalState (1) after manage" "$MS" -eq 1
assert "synthetic withdraw unmanages (list 0)" "$UM" -eq 0
assert "WM_STATE is WithdrawnState (0) after unmanage" "$WS" -eq 0
if kill -0 $WM 2>/dev/null; then echo "PASS: wm alive"; else echo "FAIL: wm dead"; fail=1; fi

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
