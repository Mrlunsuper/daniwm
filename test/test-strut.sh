#!/bin/bash
# test-strut.sh - test EWMH Struts and Dock handling in daniwm
set -u
D=:93
export DISPLAY=$D
TDIR=$(dirname "$0")
H=$(mktemp -d)
mkdir -p "$H/.config/daniwm"
cat > "$H/.config/daniwm/config" <<'CFG'
bar_on = 0
gaps_on = 1
gap_outer = 10
gap_inner = 8
workspaces = 3
CFG

fail=0
assert() {
    local desc=$1; shift
    if "$@"; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
assert_eq() {
    local desc=$1 got=$2 want=$3
    if [ "$got" -eq "$want" ]; then echo "PASS: $desc"; else echo "FAIL: $desc (got $got, want $want)"; fail=1; fi
}
assert_contains() {
    local desc=$1 str=$2 substr=$3
    if [[ "$str" == *"$substr"* ]]; then echo "PASS: $desc"; else echo "FAIL: $desc (wanted '$substr' in '$str')"; fail=1; fi
}
geom() {
    xdotool getwindowgeometry --shell "$1" 2>/dev/null | awk -F= '/^X=/{x=$2} /^Y=/{y=$2} /^WIDTH=/{w=$2} /^HEIGHT=/{h=$2} END{print x+0, y+0, w+0, h+0}'
}

DOCK=""
trap 'kill $DOCK $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

# 1. Before dock: workarea should be full screen (1280x800 for each of 3 ws)
wa=$(xprop -root _NET_WORKAREA 2>/dev/null | awk -F= '{print $2}' | tr -d ' \n')
echo "workarea initial: $wa"
assert_contains "initial workarea has y=0" "$wa" "0,0,1280,800"

# 2. Start a top dock (height=40)
[ -x "$TDIR/dock-helper" ] || gcc -O2 -o "$TDIR/dock-helper" "$TDIR/dock-helper.c" -lX11
"$TDIR/dock-helper" 40 0 0 0 1280 40 &
DOCK=$!
sleep 1

# Check updated workarea: y should be 40, h should be 760
wa=$(xprop -root _NET_WORKAREA 2>/dev/null | awk -F= '{print $2}' | tr -d ' \n')
echo "workarea with top dock: $wa"
assert_contains "workarea updated for top dock" "$wa" "0,40,1280,760"

# 3. Spawn xterm via Super+Return
xdotool key super+Return
sleep 1
xid=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -n1)
read -r x y w h <<< "$(geom "$xid")"
echo "xterm geom with dock: x=$x y=$y w=$w h=$h"
# y should be mons.y + top_strut + gap_outer + gap_inner/2 = 0 + 40 + 10 + 4 = 54
assert_eq "xterm y respects top strut (y=54)" "$y" 54

# 4. Dock is not affected by workspace switch
xdotool key super+2
sleep 0.5
assert "dock alive on ws2" kill -0 "$DOCK"

xdotool key super+1
sleep 0.5

# 5. Super+q kills xterm, not dock
xdotool key super+q
sleep 0.5
assert "dock alive after super+q" kill -0 "$DOCK"
n_xterms=$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)
assert_eq "xterm killed by super+q" "$n_xterms" 0

# Spawn xterm again
xdotool key super+Return
sleep 1
xid=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -n1)

# 6. Kill dock -> xterm immediately re-expands from y=54 to y=14
kill "$DOCK"
wait "$DOCK" 2>/dev/null || true
DOCK=""
sleep 1

read -r x y w h <<< "$(geom "$xid")"
echo "xterm geom after dock exit: x=$x y=$y w=$w h=$h"
# y is now 0 + 0 + gap_outer + gap_inner/2 = 14
assert_eq "xterm expanded after dock killed (y=14)" "$y" 14

wa=$(xprop -root _NET_WORKAREA 2>/dev/null | awk -F= '{print $2}' | tr -d ' \n')
echo "workarea restored: $wa"
assert_contains "workarea restored after dock killed" "$wa" "0,0,1280,800"

exit $fail
