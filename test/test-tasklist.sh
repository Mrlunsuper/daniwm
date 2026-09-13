#!/bin/bash
# test-tasklist.sh - Awesome-style task buttons: left-click focuses,
# middle-click closes. Uses a minimal bar (clock only) so button
# geometry is predictable: tasks start ~x=290, two buttons ~430px each.
set -u
TDIR=$(dirname "$0")
# shellcheck source=find_display.sh
. "$TDIR/find_display.sh"   # sets D to a free display number
export DISPLAY=$D

fail=0
assert() { # assert <desc> <test...>
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc";
    else echo "FAIL: $desc"; fail=1; fi
}
nall() { xdotool search --class xterm 2>/dev/null | wc -l; }
active_dec() { # focused window id in decimal
    local hex
    hex=$(xprop -root _NET_ACTIVE_WINDOW 2>/dev/null | awk '{print $NF}')
    [ -n "$hex" ] && [ "$hex" != "not" ] && printf "%d" "$hex" || echo 0
}
wait_n() { # wait_n <count> <timeout_s>
    local want=$1 timeout=$2 i=0
    while [ $i -lt $(( timeout * 2 )) ]; do
        [ "$(nall)" -eq "$want" ] && return 0
        sleep 0.5; i=$(( i + 1 ))
    done
    return 1
}
settle() { sleep 1; }

trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
# Hermetic HOME (cf. F01) with a clock-only bar for stable geometry.
H=$(mktemp -d)
unset XDG_CONFIG_HOME
mkdir -p "$H/.config/daniwm"
printf 'bar_modules = clock\n' > "$H/.config/daniwm/config"
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

xdotool key super+Return; sleep 1
idA=$(xdotool search --onlyvisible --class xterm | tail -1)
xdotool key super+Return; sleep 1
idB=$(xdotool search --onlyvisible --class xterm | grep -v "^${idA}$" | tail -1)
wait_n 2 60 || { echo "FAIL: 2 xterms never appeared"; exit 1; }
echo "PASS: spawned A=$idA B=$idB"
assert "B focused after spawn" "$(active_dec)" -eq "$idB"

# Left-click first task button -> focus jumps to A.
xdotool mousemove 400 12 click 1; settle
assert "click task 1 focuses A" "$(active_dec)" -eq "$idA"

# Left-click second task button -> focus back to B.
xdotool mousemove 1000 12 click 1; settle
assert "click task 2 focuses B" "$(active_dec)" -eq "$idB"

# Middle-click first task button -> A closes, B keeps focus.
xdotool mousemove 400 12 click 2
wait_n 1 60 || { echo "FAIL: middle-click did not close A"; exit 1; }
echo "PASS: middle-click closed A"
assert "B focused after close" "$(active_dec)" -eq "$idB"

# Phase 2: fixed width (bar_task_w=200 -> buttons [~291,491] [491,691]).
kill $WM 2>/dev/null
pkill -x xterm 2>/dev/null; sleep 1
printf 'bar_modules = clock\nbar_task_w = 200\n' > "$H/.config/daniwm/config"
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
xdotool key super+Return; sleep 1
idC=$(xdotool search --onlyvisible --class xterm | tail -1)
xdotool key super+Return; sleep 1
idD=$(xdotool search --onlyvisible --class xterm | grep -v "^${idC}$" | tail -1)
wait_n 2 60 || { echo "FAIL: phase2 terms never appeared"; exit 1; }
echo "PASS: phase2 spawned C=$idC D=$idD"

xdotool mousemove 370 12 click 1; settle
assert "fixed task 1 focuses C" "$(active_dec)" -eq "$idC"
xdotool mousemove 570 12 click 1; settle
assert "fixed task 2 focuses D" "$(active_dec)" -eq "$idD"
# Clicking the empty area past fixed buttons is a no-op.
xdotool mousemove 950 12 click 1; settle
assert "click past tasks is no-op" "$(active_dec)" -eq "$idD"

if [ "$fail" -ne 0 ]; then echo "RESULT: FAIL"; else echo "RESULT: PASS"; fi
exit $fail
