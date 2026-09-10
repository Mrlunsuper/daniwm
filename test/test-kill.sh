#!/bin/bash
# test-kill.sh - Super+q kills the focused window, WM survives and respawns.
# Usage: ./test-kill.sh (needs Xvfb, xterm, xdotool)
set -u
D=:94
export DISPLAY=$D
TDIR=$(dirname "$0")
H=$(mktemp -d)

fail=0
assert() { # assert <desc> <test...>
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
nvis() { xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l; }
waitfor() { # waitfor <n>
    i=0; while [ "$(nvis)" -ne "$1" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
}
alive() { # alive <stage>
    if kill -0 $WM 2>/dev/null; then echo "PASS: wm alive ($1)"; else echo "FAIL: wm dead ($1)"; fail=1; fi
}

trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

xdotool key super+Return; sleep 0.5
xdotool key super+Return
waitfor 2
assert "boot: 2 visible" "$(nvis)" -eq 2

xdotool key super+q
waitfor 1
assert "super+q kills one" "$(nvis)" -eq 1
alive "after first kill"

xdotool key super+q
waitfor 0
assert "super+q kills last" "$(nvis)" -eq 0
alive "after killing last"

xdotool key super+Return
waitfor 1
assert "respawn works" "$(nvis)" -eq 1
alive "after respawn"

exit $fail
