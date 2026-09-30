#!/bin/bash
# test-click-focus.sh - audit-0930 #2: focus = click must focus tiled
# windows on a plain click; focus = hover keeps its old behavior.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)
fail=0
assert() { local desc=$1; shift; if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi; }
trap 'kill $WM $XA $XB $XVFB 2>/dev/null; rm -rf "$H"' EXIT INT TERM
WM=0; XA=0; XB=0
Xvfb $D -screen 0 1280x800x24 & XVFB=$!
sleep 1
act() { xprop -root _NET_ACTIVE_WINDOW | awk '{print $NF}'; }
foc() { xdotool getwindowfocus 2>/dev/null; }
mkdir -p "$H/.config/daniwm"

run_mode() { # run_mode <mode>
    printf 'focus = %s\n' "$1" > "$H/.config/daniwm/config"
    xdotool mousemove 640 5
    HOME=$H "${WM_BIN:-$TDIR/../daniwm}" & WM=$!
    sleep 1
    xterm -title CFA & XA=$!; sleep 1; xterm -title CFB & XB=$!; sleep 1.5
    # pointer lands inside the left window after spawn: go right first so
    # every move below is a real crossing (hover) / a real click target.
    xdotool mousemove 1000 400 click 1; sleep 1
    xdotool mousemove 200 400 click 1; sleep 1; L=$(act)
    xdotool mousemove 1000 400 click 1; sleep 1; R=$(act); RF=$(foc)
    assert "$1: click left/right yields different active" "$L" != "$R"
    assert "$1: real input focus follows click" "$(printf 0x%x "$RF")" = "$R"
    kill $XA $XB 2>/dev/null; sleep 0.5
    kill $WM 2>/dev/null; wait $WM 2>/dev/null
}
run_mode click
run_mode hover
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
