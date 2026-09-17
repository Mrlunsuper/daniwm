#!/bin/bash
# test-focus-steal.sh - a closing context menu must not steal focus from a
# dialog that just opened.
# Real case: right-click an image in the browser, pick Save image as. The
# menu holds a pointer grab. The portal dialog maps and the WM focuses it.
# Then the menu closes, and X reports the pointer entering the window under
# it. The WM must not follow that, because the pointer never moved.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
. "$TDIR/toolchain.sh"
export DISPLAY=$D
H=$(mktemp -d)

fail=0
assert() { local desc=$1; shift; if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi; }
active() { xprop -root _NET_ACTIVE_WINDOW 2>/dev/null | sed -n 's/.*# *\(0x[0-9a-f]*\).*/\1/p'; }
norm() { printf '0x%x\n' "$1"; }

# kill only real pids: "kill 0" would signal the whole process group and
# take run-all.sh down with it.
cleanup() {
    for p in $MENU $DLG $WM $XVFB; do [ "$p" -gt 0 ] 2>/dev/null && kill "$p" 2>/dev/null; done
    rm -rf "$H"
}
trap 'cleanup; exit $fail' EXIT INT TERM
MENU=0; DLG=0; WM=0; XVFB=0

for b in menu-helper dialog-helper; do
    [ -x "$TDIR/$b" ] || ${CC} ${CFLAGS} -o "$TDIR/$b" "$TDIR/$b.c" -lX11
done

Xvfb $D -screen 0 1280x800x24 & XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" & WM=$!
sleep 1

xdotool key super+Return; sleep 2
A=$(xdotool search --onlyvisible --class xterm | head -n1)
[ -n "$A" ] || { echo "FAIL: no xterm"; exit 1; }

# pointer parked inside A and NOT moved again for the rest of the test
eval "$(xdotool getwindowgeometry --shell "$A")"
PX=$((X + WIDTH/2)); PY=$((Y + HEIGHT/2))
xdotool mousemove $PX $PY; sleep 0.8
assert "A focused by hover" "$(active)" = "$(norm "$A")"

# context menu opens over the pointer and grabs it
"$TDIR/menu-helper" 2500 >/dev/null 2>&1 & MENU=$!
sleep 0.8
# dialog maps while the menu is still up
"$TDIR/dialog-helper" 6000 >/dev/null 2>&1 & DLG=$!
sleep 1.2
DID=$(xdotool search --name dialog-helper | head -n1)
[ -n "$DID" ] || { echo "FAIL: dialog did not map"; exit 1; }
assert "dialog takes focus when it opens" "$(active)" = "$(norm "$DID")"

# menu closes; pointer has not moved
wait $MENU 2>/dev/null; MENU=0
sleep 1
assert "closing menu does not steal focus from the dialog" "$(active)" = "$(norm "$DID")"

# a real mouse move must still drive focus: cross into the dialog, then
# back into A. Moving inside one window makes no crossing event at all.
eval "$(xdotool getwindowgeometry --shell "$DID" | sed 's/^/D/')"
xdotool mousemove $((DX + DWIDTH/2)) $((DY + DHEIGHT/2)); sleep 0.6
xdotool mousemove $((X + 20)) $((Y + 20)); sleep 0.8
assert "hover focus still works after a real move" "$(active)" = "$(norm "$A")"

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
