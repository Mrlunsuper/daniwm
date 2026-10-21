#!/bin/bash
# Scratchpad must preserve monocle's visible tile and restore prior focus.
set -euo pipefail
TDIR=$(cd "$(dirname "$0")" && pwd)
. "$TDIR/find_display.sh"
export DISPLAY=$D
export SHELL=/bin/sh
H=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    rm -rf "$H"
}
trap cleanup EXIT
Xvfb "$D" -screen 0 1280x800x24 >"$H/xvfb.log" 2>&1 &
PIDS+=("$!")
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" >"$H/wm.log" 2>&1 &
PIDS+=("$!")
sleep 1
# Keep the pointer out of client windows (sloppy focus).
xdotool mousemove 1279 799
for title in scratch-test-a scratch-test-b; do
    xterm -title "$title" -e /bin/sh -c 'sleep 120' >"$H/$title.log" 2>&1 &
    PIDS+=("$!")
    sleep 0.5
done
A=$(xdotool search --name '^scratch-test-b$' | head -1)
B=$(xdotool search --name '^scratch-test-a$' | head -1)
active() { xprop -root _NET_ACTIVE_WINDOW | awk '{print $NF}'; }
assert_active() {
    local expected
    printf -v expected '0x%x' "$1"
    [ "$(active)" = "$expected" ] || { echo "FAIL: $2 (active=$(active), expected=$expected)"; exit 1; }
    echo "PASS: $2"
}
xdotool key super+m
sleep 0.3
# Clients are appended: the older client is the list head. Select the newer.
xdotool windowactivate "$A"
sleep 0.5
assert_active "$A" 'select non-head monocle window'
for round in 1 2; do
    xdotool key super+s
    sleep 1
    S=$(xdotool search --classname scratchpad | head -1)
    assert_active "$S" "scratchpad focused (round $round)"
    xdotool search --onlyvisible --name '^scratch-test-b$' | grep -qx "$A"
    if xdotool search --onlyvisible --name '^scratch-test-a$' | grep -qx "$B"; then
        echo 'FAIL: monocle switched to list head underneath scratchpad'
        exit 1
    fi
    echo "PASS: underlying monocle window preserved (round $round)"
    xdotool key super+s
    sleep 0.5
    assert_active "$A" "previous focus restored (round $round)"
done
# Actually destroying the scratchpad must use the same stable return path,
# not briefly show the current tile and then switch to the client-list head.
xdotool key super+s
sleep 0.5
S=$(xdotool search --classname scratchpad | head -1)
xdotool mousemove 1279 799
sleep 0.2
xdotool windowclose "$S"
sleep 0.5
assert_active "$A" 'destroy scratchpad restores visible non-head tile'
# Move away and back: stale crossing events must not have changed monocle.
xdotool mousemove 1279 799
xdotool mousemove 640 400
sleep 0.3
assert_active "$A" 'focus stays stable after scratchpad destruction'
# A stale return XID must fall back safely if that client closes.
xdotool key super+s
sleep 0.5
xdotool windowclose "$A"
sleep 0.5
xdotool key super+s
sleep 0.5
assert_active "$B" 'closed return target falls back to remaining client'
echo 'RESULT: PASS'
