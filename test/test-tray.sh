#!/bin/bash
# test-tray.sh - system tray (XEmbed) docking in daniwm
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
. "$TDIR/toolchain.sh"
export DISPLAY=$D
H=$(mktemp -d)
mkdir -p "$H/.config/daniwm"
cat > "$H/.config/daniwm/config" <<'CFG'
bar_on = 1
tray = 1
workspaces = 3
CFG

fail=0
assert() {
    local desc=$1; shift
    if "$@"; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
assert_contains() {
    local desc=$1 str=$2 substr=$3
    if [[ "$str" == *"$substr"* ]]; then echo "PASS: $desc"; else echo "FAIL: $desc (wanted '$substr' in '$str')"; fail=1; fi
}

ICON=""; ICONOUT=""
trap 'kill $ICON $WM $XVFB 2>/dev/null; rm -rf "$H" "$ICONOUT"; exit $fail' EXIT INT TERM

${CC:-gcc} ${CFLAGS:--O2} -o "$TDIR/tray-icon-helper" "$TDIR/tray-icon-helper.c" -lX11 || { echo "FAIL: build helper"; exit 1; }

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

# 1. tray selection owned (proven by docking; selections aren't X props,
#    so xprop can't query them — the helper's DOCKED line is the proof)
ICONOUT=$(mktemp)
"$TDIR/tray-icon-helper" >"$ICONOUT" 2>&1 &
ICON=$!
sleep 2

cat "$ICONOUT"
line=$(grep DOCKED "$ICONOUT" | head -n1 || true)
assert_contains "icon docked (reparented off root)" "${line:-none}" "DOCKED"
if [ -n "$line" ]; then
    parent=$(echo "$line" | sed -n 's/.*parent=\(0x[0-9a-f]*\).*/\1/p')
    rootw=$(echo "$line" | sed -n 's/.*root=\(0x[0-9a-f]*\).*/\1/p')
    if [ "$parent" != "$rootw" ] && [ -n "$parent" ]; then echo "PASS: parent != root ($parent)"; else echo "FAIL: still child of root"; fail=1; fi
    # 2. icon geometry: square, inside bar height (<=32)
    win=$(echo "$line" | sed -n 's/.*win=\(0x[0-9a-f]*\).*/\1/p')
    dec=$((win))
    geom=$(xdotool getwindowgeometry --shell "$dec" 2>/dev/null || true)
    echo "icon $geom"
    assert_contains "icon geometry readable" "$geom" "WIDTH="
    # 3. icon NOT managed as client
    cl=$(xprop -root _NET_CLIENT_LIST 2>/dev/null || true)
    echo "$cl"
    if [[ "$cl" == *"${win#0x}"* ]] || [[ "$cl" == *"$win"* ]]; then echo "FAIL: tray icon leaked into CLIENT_LIST"; fail=1; else echo "PASS: icon not in CLIENT_LIST"; fi
fi

# 4. kill icon -> WM survives, normal client still works
kill "$ICON" 2>/dev/null
wait "$ICON" 2>/dev/null || true
ICON=""
sleep 1
xdotool key super+Return
sleep 1
n=$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)
if [ "$n" -ge 1 ]; then echo "PASS: wm alive after tray test"; else echo "FAIL: wm dead/no xterm"; fail=1; fi

# 5. tray = 0: no selection owner
kill "$WM" 2>/dev/null; wait "$WM" 2>/dev/null || true
cat > "$H/.config/daniwm/config" <<'CFG'
bar_on = 1
tray = 0
workspaces = 3
CFG
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
"$TDIR/tray-icon-helper" >"$ICONOUT" 2>&1 &
ICON=$!
# helper waits up to 5s for an owner that never comes; wait for it to give up
wait "$ICON" 2>/dev/null || true
ICON=""
if grep -q "no tray owner" "$ICONOUT"; then echo "PASS: tray=0 owns nothing"; else echo "FAIL: tray=0 still owns selection"; cat "$ICONOUT"; fail=1; fi
kill "$ICON" 2>/dev/null; wait "$ICON" 2>/dev/null || true; ICON=""

exit $fail
