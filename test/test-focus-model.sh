#!/bin/bash
# test-focus-model.sh - T-H2.1 lock: external focus syncs sel/active.
#
# Fixed: FocusIn updates sel/ws_sel, clears urgency, refreshes borders,
# bar and _NET_ACTIVE_WINDOW without raise or arrange. FocusOut never
# steals back. Keys/buttons still drive focus() directly.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

# Fixed behavior: external XSetInputFocus syncs sel/active.
EXPECTED_SYNC=1

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

OUT=$(python3 "$TDIR/focus-model.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
SYNC=$(echo "$OUT" | sed -n 's/^SYNC=//p' | tail -n1)
STORM=$(echo "$OUT" | sed -n 's/^STORM_SYNC=//p' | tail -n1)
[ -n "${SYNC:-}" ] || { echo "FAIL: no SYNC from helper"; exit 1; }
[ -n "${STORM:-}" ] || { echo "FAIL: no STORM_SYNC from helper"; exit 1; }

assert "external focus change syncs sel/active" "$SYNC" -eq "$EXPECTED_SYNC"
assert "50-flip FocusIn storm tracks last window" "$STORM" -eq 1

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
