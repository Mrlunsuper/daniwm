#!/bin/bash
# test-rename-stuck.sh - audit-0930 #9: a rename child killed without
# waking the parent (kill -9, so USR1 never arrives) must not leave
# pending_idx stuck; the next Super+Shift+n runs instead of warning.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)
fail=0
assert() { local desc=$1; shift; if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi; }
trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"' EXIT INT TERM
WM=0
Xvfb $D -screen 0 1280x800x24 & XVFB=$!
sleep 1
mkdir -p "$H/.config/daniwm"
# eval runs in the wrapper child: kill -9 $$ kills the wrapper so the
# final "kill -USR1 $PPID" never runs; pending_idx stays set forever.
printf 'rename_cmd = kill -9 $$\n' > "$H/.config/daniwm/config"
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" 2>"$H/wm.err" & WM=$!
sleep 1
kill -0 $WM 2>/dev/null || { echo "FAIL: wm did not start"; exit 1; }
xdotool key super+shift+n; sleep 1
assert "first rename started (no warning)" "! grep -q 'already in progress' $H/wm.err"
sleep 2 # hard-killed child: no USR1 ever arrives
xdotool key super+shift+n; sleep 1
if grep -q "already in progress" "$H/wm.err"; then
    echo "FAIL: second rename stuck on dead child"; fail=1
else echo "PASS: second rename runs (dead pending cleared)"; fi
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
