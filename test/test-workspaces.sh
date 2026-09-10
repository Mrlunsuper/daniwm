#!/bin/bash
# test-workspaces.sh - workspaces=N bounds, mod-only bind rebuild, reload fold.
# Usage: ./test-workspaces.sh (needs Xvfb, xterm, xdotool)
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)
mkdir -p "$H/.config/daniwm"


fail=0
assert() { # assert <desc> <test...>
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
nvis() { xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l; }
spawnwait() { # spawnwait <n>
    xdotool key super+Return
    i=0; while [ "$(nvis)" -lt "$1" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
}
alive() { # alive <stage>
    if kill -0 $WM 2>/dev/null; then echo "PASS: wm alive ($1)"; else echo "FAIL: wm dead ($1)"; fail=1; fi
}

trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

# --- part 1: workspaces = 3 bounds ---
echo "workspaces = 3" > "$H/.config/daniwm/config"
Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

spawnwait 1
assert "ws3 boot: 1 visible" "$(nvis)" -eq 1
xdotool key super+4; sleep 1
assert "ws4 with NWS=3 is no-op" "$(nvis)" -eq 1
xdotool key super+3; sleep 1
assert "ws3 empty" "$(nvis)" -eq 0
xdotool key super+1; sleep 1
assert "back to ws1" "$(nvis)" -eq 1
xdotool key super+Shift+3; sleep 1
assert "move to ws3 follows" "$(nvis)" -eq 1
xdotool key super+1; sleep 1
assert "ws1 empty after move" "$(nvis)" -eq 0
alive "part1 end"
kill "$WM"; sleep 1
pkill -x xterm 2>/dev/null; sleep 1

# --- part 2: mod-only config rebuilds default binds on alt ---
echo "mod = alt" > "$H/.config/daniwm/config"
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
alive "part2 boot"
xdotool key super+Return; sleep 1
assert "super dead under mod=alt" "$(nvis)" -eq 0
xdotool key alt+Return
i=0; while [ "$(nvis)" -lt 1 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
assert "alt+Return spawns" "$(nvis)" -eq 1
xdotool key alt+2; sleep 1
assert "alt+2 switches" "$(nvis)" -eq 0
xdotool key alt+1; sleep 1
assert "alt+1 back" "$(nvis)" -eq 1
alive "part2 end"
kill "$WM"; sleep 1
pkill -x xterm 2>/dev/null; sleep 1

# --- part 3: reload shrink folds ws3 into last (ws2) ---
rm "$H/.config/daniwm/config"
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
alive "part3 boot, no config"
spawnwait 1
xdotool key super+3; sleep 1
spawnwait 1
printf 'workspaces = 2\n' > "$H/.config/daniwm/config"
xdotool key super+Shift+r; sleep 1.5
alive "after shrink reload"
xdotool key super+1; sleep 1
assert "ws1 keeps its win after fold" "$(nvis)" -eq 1
xdotool key super+2; sleep 1
assert "ws3 folded into ws2" "$(nvis)" -eq 1
xdotool key super+3; sleep 1
assert "ws3 no-op after shrink" "$(nvis)" -eq 1

exit $fail
