#!/bin/bash
# test-focus-model.sh - T-H2.0 focus-desync observation harness (test only).
#
# Problem (H2, audit:69-74): FocusIn/FocusOut selected in client.c:459 but
# dropped in main.c:682 default arm. Real input focus moves without WM
# knowledge, so WM sel / _NET_ACTIVE_WINDOW points elsewhere.
#
# XFAIL STATUS: asserts the current desync (EXPECTED_SYNC=0) so
# `make check` stays green. T-H2.1 flips ONLY EXPECTED_SYNC to 1.
# No WM code changes. Read-only probe.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

# T-H2.1: flip to 1 (fixed: external XSetInputFocus syncs sel/active).
EXPECTED_SYNC=0

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
[ -n "${SYNC:-}" ] || { echo "FAIL: no SYNC from helper"; exit 1; }

if [ "$EXPECTED_SYNC" -eq 0 ]; then
    echo "XFAIL(T-H2.0): external focus change desyncs WM sel/active; T-H2.1 flips EXPECTED_SYNC to 1"
fi
assert "focus sync state is $EXPECTED_SYNC" "$SYNC" -eq "$EXPECTED_SYNC"

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
