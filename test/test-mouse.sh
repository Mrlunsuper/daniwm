#!/bin/bash
# test-mouse.sh - Mod+Left move, Mod+Right resize, float-promote, deadzone click.
# Usage: ./test-mouse.sh (needs Xvfb, xterm, xdotool)
set -u
D=:97
export DISPLAY=$D
TDIR=$(dirname "$0")
H=$(mktemp -d)

fail=0
assert() { # assert <desc> <test...>
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
geom() { xdotool getwindowgeometry --shell "$1" 2>/dev/null | awk -F= '/^X=/{x=$2} /^Y=/{y=$2} /^WIDTH=/{w=$2} /^HEIGHT=/{h=$2} END{print x+0, y+0, w+0, h+0}'; }
center() { xdotool getwindowgeometry --shell "$1" 2>/dev/null | awk -F= '/^X=/{x=$2} /^Y=/{y=$2} /^WIDTH=/{w=$2} /^HEIGHT=/{h=$2} END{print int(x+w/2), int(y+h/2)}'; }
# drag <button> <dx> <dy>: separate calls + settles, so the WM's pointer
# grab round-trip lands before motion (one-shot press/move/release races it).
drag() {
    xdotool keydown super; xdotool mousedown "$1"; sleep 0.4
    xdotool mousemove_relative -- "$2" "$3"; sleep 0.4
    xdotool mouseup "$1"; xdotool keyup super; sleep 0.8
}

trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

xdotool key super+Return
i=0; while [ "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -lt 1 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
id=$(xdotool search --class xterm 2>/dev/null | head -n1)
read -r x0 y0 w0 h0 <<< "$(geom "$id")"

read -r cx cy <<< "$(center "$id")"
xdotool mousemove $cx $cy; sleep 0.3
drag 1 120 80
read -r x1 y1 w1 h1 <<< "$(geom "$id")"
assert "move +120+80" "$x1" -eq $((x0+120)) -a "$y1" -eq $((y0+80)) -a "$w1" -eq "$w0" -a "$h1" -eq "$h0"

read -r cx cy <<< "$(center "$id")"
xdotool mousemove $cx $cy; sleep 0.3
drag 3 60 40
read -r x2 y2 w2 h2 <<< "$(geom "$id")"
assert "resize +60+40" "$x2" -eq "$x1" -a "$y2" -eq "$y1" -a "$w2" -eq $((w1+60)) -a "$h2" -eq $((h1+40))

# dragged window promoted to float: monocle keeps its geometry
xdotool key super+m; sleep 1
read -r xm ym wm hm <<< "$(geom "$id")"
assert "dragged floats in monocle" "$xm" -eq "$x2" -a "$ym" -eq "$y2" -a "$wm" -eq "$w2" -a "$hm" -eq "$h2"
xdotool key super+space; sleep 1

# deadzone: Mod+click without motion stays tiled (monocle fills area)
xdotool key super+2; sleep 1
xdotool key super+Return
i=0; while [ "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -lt 1 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
id2=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -n1)
read -r cx cy <<< "$(center "$id2")"
xdotool mousemove $cx $cy; sleep 0.3
xdotool keydown super; xdotool mousedown 1; sleep 0.4; xdotool mouseup 1; xdotool keyup super; sleep 0.8
xdotool key super+m; sleep 1
read -r xk yk wk hk <<< "$(geom "$id2")"
assert "click without motion stays tiled" "$xk" -eq 10 -a "$yk" -eq 34 -a "$wk" -eq 1256 -a "$hk" -eq 752

exit $fail
