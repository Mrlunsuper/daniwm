#!/bin/bash
# test-clamp-float.sh - T-L2: floating geometry stays clamped.
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

OUT=$(python3 "$TDIR/clamp-float.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
COK=$(echo "$OUT" | sed -n 's/^CLAMP_OK=//p' | tail -n1)
MOK=$(echo "$OUT" | sed -n 's/^MANAGED_OK=//p' | tail -n1)
[ -n "${COK:-}" ] || { echo "FAIL: no CLAMP_OK"; exit 1; }
[ -n "${MOK:-}" ] || { echo "FAIL: no MANAGED_OK"; exit 1; }
assert "degenerate sizes clamp to >=1 and bounded" "$COK" -eq 1
assert "window stays managed through storm" "$MOK" -eq 1
if kill -0 $WM 2>/dev/null; then echo "PASS: wm survives clamp storm"; else echo "FAIL: wm died"; fail=1; fi

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
