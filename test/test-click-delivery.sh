#!/bin/bash
# test-click-delivery.sh - clicks on client windows must reach the APP
# untouched (clicks, Shift+click text selection, middle-click paste,
# right-click menus). The WM deliberately observes plain presses through
# no grabs and no ButtonPress selection (either would starve the app of
# button events on this X stack); it focuses via hover and raises via
# Mod+click/drag and the keyboard. This suite guards against regressions
# that swallow app clicks. WM focus assertions rely on hover (sloppy).
# Usage: ./test-click-delivery.sh (needs Xvfb, xdotool)
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

fail=0
assert() {
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

"$TDIR/click-helper" > "$H/helper.log" 2>&1 &
sleep 0.5
i=0; while ! grep -q READY "$H/helper.log" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
grep -q READY "$H/helper.log" || { echo "FAIL: helper never ready"; cat "$H/helper.log"; exit 1; }
id=$(awk '/READY/{print $2}' "$H/helper.log" | tail -n1)
echo "INFO helper win=$id"
sleep 1 # let the WM manage + tile it

center() { xdotool getwindowgeometry --shell "$1" 2>/dev/null | awk -F= '/^X=/{x=$2} /^Y=/{y=$2} /^WIDTH=/{w=$2} /^HEIGHT=/{h=$2} END{print int(x+w/2), int(y+h/2)}'; }
npress() { grep -c "^PRESS" "$H/helper.log"; }

# 1. plain left click: WM focuses AND app gets the press
read -r cx cy <<< "$(center "$id")"
n0=$(npress)
xdotool mousemove "$cx" "$cy"; sleep 0.5
xdotool click 1; sleep 0.8
assert "WM focuses clicked window (focus=$(xdotool getwindowfocus))" "$(xdotool getwindowfocus 2>/dev/null)" = "$id"
assert "app receives left click" "$(npress)" -gt "$n0"
grep "button=1" "$H/helper.log" | tail -n1

# 2. Shift+click (terminal text-selection flow): app gets press with Shift
n0=$(npress)
xdotool keydown Shift; sleep 0.2
xdotool click 1; sleep 0.8
xdotool keyup Shift; sleep 0.3
assert "app receives Shift+click" "$(npress)" -gt "$n0"
last=$(grep "^PRESS" "$H/helper.log" | tail -n1)
echo "INFO $last"
case "$last" in
  *button=1*) echo "PASS: Shift+click button ok" ;;
  *) echo "FAIL: Shift+click button ($last)"; fail=1 ;;
esac

# 3. middle click (paste flow): app gets button 2
n0=$(npress)
xdotool click 2; sleep 0.8
assert "app receives middle click" "$(npress)" -gt "$n0"
last=$(grep "^PRESS" "$H/helper.log" | tail -n1)
case "$last" in
  *button=2*) echo "PASS: middle-click button ok" ;;
  *) echo "FAIL: middle-click button ($last)"; fail=1 ;;
esac

# 4. right click: app gets button 3
n0=$(npress)
xdotool click 3; sleep 0.8
assert "app receives right click" "$(npress)" -gt "$n0"

exit $fail
