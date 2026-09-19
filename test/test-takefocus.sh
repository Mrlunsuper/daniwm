#!/bin/bash
# test-takefocus.sh - T-H2.3: endorsing clients get WM_TAKE_FOCUS, others
# keep the XSetInputFocus path.
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

OUT=$(python3 "$TDIR/takefocus.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
GOT=$(echo "$OUT" | sed -n 's/^TAKE_FOCUS_GOT=//p' | tail -n1)
NOK=$(echo "$OUT" | sed -n 's/^NORMAL_FOCUS_OK=//p' | tail -n1)
[ -n "${GOT:-}" ] || { echo "FAIL: no TAKE_FOCUS_GOT"; exit 1; }
[ -n "${NOK:-}" ] || { echo "FAIL: no NORMAL_FOCUS_OK"; exit 1; }

assert "endorser receives WM_TAKE_FOCUS ClientMessage" "$GOT" -eq 1
assert "non-endorsing path keeps XSetInputFocus" "$NOK" -eq 1

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
