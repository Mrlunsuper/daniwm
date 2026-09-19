#!/bin/bash
# test-noinput.sh - T-H2.2: WM_HINTS input=False never gets XSetInputFocus.
# sel/active may name the NoInput window, but real input stays elsewhere.
# Normal windows unchanged.
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

OUT=$(python3 "$TDIR/noinput.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
NO_OK=$(echo "$OUT" | sed -n 's/^NOINPUT_OK=//p' | tail -n1)
N_OK=$(echo "$OUT" | sed -n 's/^NORMAL_OK=//p' | tail -n1)
[ -n "${NO_OK:-}" ] || { echo "FAIL: no NOINPUT_OK from helper"; exit 1; }
[ -n "${N_OK:-}" ] || { echo "FAIL: no NORMAL_OK from helper"; exit 1; }

assert "NoInput window never receives XSetInputFocus" "$NO_OK" -eq 1
assert "normal window still receives input focus" "$N_OK" -eq 1

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
