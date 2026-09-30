#!/bin/bash
# test-gap-cap.sh - audit-0930 #8: gap_inc is capped and monocle clamps
# its size, so spamming super+equal never breaks the window.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)
fail=0
assert() { local desc=$1; shift; if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi; }
trap 'kill $WM $XT $XVFB 2>/dev/null; rm -rf "$H"' EXIT INT TERM
WM=0; XT=0
Xvfb $D -screen 0 1280x800x24 & XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" 2>"$H/wm.err" & WM=$!
sleep 1
xterm -title GAPT & XT=$!
sleep 1.5
W=$(xdotool search --class xterm | head -1)
xdotool key super+m; sleep 0.3   # monocle
for _ in $(seq 1 200); do xdotool key super+equal; done
sleep 1
kill -0 $WM 2>/dev/null; assert "wm alive" $? -eq 0
assert "window still viewable" "$(xdotool search --onlyvisible --class xterm | wc -l)" -ge 1
G=$(xdotool getwindowgeometry "$W" | awk '/Geometry/{print $2}')
echo "geometry=$G"
w=${G%x*}; h=${G#*x}
assert "window fits the screen with a usable size" "$w" -ge 100 -a "$h" -ge 100 -a "$w" -le 1280 -a "$h" -le 800
if grep -q BadValue "$H/wm.err"; then echo "FAIL: BadValue in wm stderr"; fail=1; else echo "PASS: no BadValue"; fi
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
