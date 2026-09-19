#!/bin/bash
# test-xerr.sh - T-H3A: trapped-error helper logs BadWindow and continues.
# No WM behavior change (helper exists, unused by WM paths yet).
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
. "$TDIR/toolchain.sh"
export DISPLAY=$D
H=$(mktemp -d)

fail=0
assert() { local desc=$1; shift; if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi; }

cleanup() {
    kill $XVFB 2>/dev/null
    rm -rf "$H" "$TDIR/xerr-test"
}
trap 'cleanup; exit $fail' EXIT INT TERM
XVFB=0

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1

${CC} ${CFLAGS} -Wall -Wextra -o "$TDIR/xerr-test" "$TDIR/xerr-test.c" "$TDIR/../src/xerr.c" -lX11 \
    >"$H/build.log" 2>&1 || { echo "FAIL: build xerr-test"; cat "$H/build.log"; exit 1; }
echo "PASS: xerr-test builds warning-free"

OUT=$("$TDIR/xerr-test" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
cat "$H/stderr.log"
[ "$RC" -eq 0 ] || { echo "FAIL: xerr-test exited rc=$RC (must continue after trap)"; exit 1; }
assert "process continues after trapped error" "$OUT" = "ALIVE=1"
if grep -q "trapped X error" "$H/stderr.log"; then
    echo "PASS: trapped error logged with request codes"
else
    echo "FAIL: no trapped-error log line"; fail=1
fi

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
