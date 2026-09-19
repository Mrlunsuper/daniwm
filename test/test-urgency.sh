#!/bin/bash
# test-urgency.sh - T-M2: hint clear and DA removal clear urgency.
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

OUT=$(python3 "$TDIR/urgency.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
for k in DA_ADD_OK DA_REMOVE_OK HINT_SET_OK HINT_CLEAR_OK FOCUS_CLEAR_OK; do
    v=$(echo "$OUT" | sed -n "s/^$k=//p" | tail -n1)
    [ -n "${v:-}" ] || { echo "FAIL: no $k"; exit 1; }
    assert "$k" "$v" -eq 1
done

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
