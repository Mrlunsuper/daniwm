#!/bin/bash
# test-reload-hidden.sh - T-L4: k_reload maps no hidden window.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

fail=0
assert() { local desc=$1; shift; if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi; }

cleanup() {
    kill $HPID $WM $XVFB 2>/dev/null
    rm -rf "$H"
}
trap 'cleanup; exit $fail' EXIT INT TERM
WM=0; XVFB=0; HPID=0

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
kill -0 $WM 2>/dev/null || { echo "FAIL: wm did not start"; exit 1; }

python3 -u "$TDIR/reload-hidden.py" >"$H/out.log" 2>"$H/stderr.log" &
HPID=$!
IDS=""
for _ in $(seq 1 150); do
    sleep 0.1
    IDS=$(sed -n 's/^IDS=//p' "$H/out.log" 2>/dev/null | tail -n1)
    [ -n "${IDS:-}" ] && break
    kill -0 $HPID 2>/dev/null || break
done
[ -n "${IDS:-}" ] || { echo "FAIL: helper never mapped"; cat "$H/out.log" "$H/stderr.log"; exit 1; }
echo "IDS=$IDS"

xdotool key super+m 2>/dev/null || { echo "FAIL: xdotool super+m"; exit 1; }
sleep 1
xdotool key super+shift+r 2>/dev/null || { echo "FAIL: xdotool reload"; exit 1; }
wait $HPID
RC=$?
[ "$RC" -eq 0 ] || { echo "FAIL: helper failed (rc=$RC)"; cat "$H/out.log" "$H/stderr.log"; exit 1; }
cat "$H/out.log"
OK=$(sed -n 's/^RELOAD_OK=//p' "$H/out.log" | tail -n1)
[ -n "${OK:-}" ] || { echo "FAIL: no RELOAD_OK"; exit 1; }
assert "reload maps only sel, others stay hidden" "$OK" -eq 1

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
