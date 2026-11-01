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
trap 'cleanup' EXIT INT TERM
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
for key in W1_MESSAGE_OK W1_FOCUS_OK W1_KEY_OK \
           W2_MESSAGE_OK W2_FOCUS_OK W2_KEY_OK \
           W1_RETURN_MESSAGE_OK W1_RETURN_FOCUS_OK W1_RETURN_KEY_OK \
           NORMAL_FOCUS_OK NOINPUT_MESSAGE_OK NOINPUT_FOCUS_OK NOINPUT_ACTIVE_OK; do
    value=$(printf '%s\n' "$OUT" | sed -n "s/^${key}=//p")
    assert "$key (required result)" "$value" = 1
done

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
