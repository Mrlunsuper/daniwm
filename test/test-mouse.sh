#!/bin/bash
# test-mouse.sh - Mod+Left move, Mod+Right resize, float-promote, deadzone click.
# Usage: ./test-mouse.sh (needs Xvfb, xterm, xdotool)
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
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
assert "click without motion stays tiled" "$xk" -eq 14 -a "$yk" -eq 38 -a "$wk" -eq 1248 -a "$hk" -eq 744

# tiled Mod+Right drag resizes mfact (and stays tiled)
xdotool key super+3; sleep 1
xdotool key super+Return; sleep 1
xdotool key super+Return; sleep 1
i=0; while [ "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -lt 2 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
id_master=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -n1)
read -r xm0 ym0 wm0 hm0 <<< "$(geom "$id_master")"
read -r cx cy <<< "$(center "$id_master")"
xdotool mousemove $cx $cy; sleep 0.3
drag 3 60 0
read -r xm1 ym1 wm1 hm1 <<< "$(geom "$id_master")"
assert "tiled resize grows master" "$wm1" -gt "$wm0"

# verify master stayed tiled (in monocle, it expands to 1248 and stack is unmapped)
xdotool key super+m; sleep 1
read -r xmono ymono wmono hmono <<< "$(geom "$id_master")"
assert "resized master stays tiled in monocle" "$wmono" -eq 1248
vis=$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)
assert "stack hidden in monocle" "$vis" -eq 1

# tiled Mod+Right vertical drag is border-following (pairwise cfact):
# 1 master + 2 stack on ws4. Top drag down grows; bottom drag down is a
# clean no-op (screen edge); bottom drag up grows upward.
pickstack() { # $1 = head|tail -> top/bottom stack window id (stack: x>700)
  for id in $(xdotool search --onlyvisible --class xterm 2>/dev/null); do
    set -- $(geom $id)
    if [ "$1" -gt 700 ]; then echo "$2 $id"; fi
  done | sort -n | "$1" -n1 | awk '{print $2}'
}
xdotool key super+4; sleep 1
xdotool key super+Return; sleep 1
xdotool key super+Return; sleep 1
xdotool key super+Return; sleep 1
i=0; while [ "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -lt 3 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
topstack=$(pickstack head)
botstack=$(pickstack tail)
read -r tx0 ty0 tw0 th0 <<< "$(geom "$topstack")"
read -r cx cy <<< "$(center "$topstack")"
xdotool mousemove $cx $cy; sleep 0.3
drag 3 0 150
read -r tx1 ty1 tw1 th1 <<< "$(geom "$topstack")"
assert "top stack drag down grows" "$th1" -gt "$th0"
read -r bx0 by0 bw0 bh0 <<< "$(geom "$botstack")"
read -r cx cy <<< "$(center "$botstack")"
xdotool mousemove $cx $cy; sleep 0.3
drag 3 0 150
read -r bx1 by1 bw1 bh1 <<< "$(geom "$botstack")"
read -r tx1b ty1b tw1b th1b <<< "$(geom "$topstack")"
assert "bottom drag down no-op (h)" "$bh1" -eq "$bh0"
assert "bottom drag down no-op (y)" "$by1" -eq "$by0"
assert "top untouched by bottom no-op" "$th1b" -eq "$th1"
read -r cx cy <<< "$(center "$botstack")"
xdotool mousemove $cx $cy; sleep 0.3
drag 3 0 -150
read -r bx2 by2 bw2 bh2 <<< "$(geom "$botstack")"
read -r tx2 ty2 tw2 th2 <<< "$(geom "$topstack")"
assert "bottom drag up grows" "$bh2" -gt "$bh1"
assert "bottom drag up moves top edge up" "$by2" -lt "$by1"
assert "top shrinks as bottom grows" "$th2" -lt "$th1"

exit $fail
