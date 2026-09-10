#!/bin/sh
# RandR hotplug: WM must survive a screen resize and re-tile clients.
# Needs Xvfb + xrandr + xdotool. Xvfb has a single output, so a real
# connect/disconnect can't be simulated; --fb resize exercises the same
# RRScreenChangeNotify -> on_monitors_changed() path (multi-monitor
# add/remove shares it: Xinerama re-query + bar/strut/arrange refresh).
set -u
D=:99
Xvfb $D -screen 0 1280x800x24 >/dev/null 2>&1 & XVFB=$!
sleep 1
export DISPLAY=$D
./daniwm >/dev/null 2>&1 & WM=$!
sleep 1
xterm & sleep 1
pass=0; fail=0
ok() { # ok <desc> <cond...>
    d=$1; shift
    if "$@"; then echo "PASS: $d"; pass=$((pass+1)); else echo "FAIL: $d"; fail=$((fail+1)); fi
}
G() { DISPLAY=$D xdotool search --onlyvisible --class xterm getwindowgeometry 2>/dev/null; }
BEFORE_W=$(G | grep Geometry | head -n1)
echo "before: $BEFORE_W"
DISPLAY=$D xrandr --fb 1024x600 >/dev/null 2>&1; sleep 1
ok "wm survives RandR resize" kill -0 $WM 2>/dev/null
AFTER=$(G | grep Geometry | head -n1)
echo "after: $AFTER"
# 1280x800 single master: 1248x744; 1024x600 -> 992x544
echo "$AFTER" | grep -q "992x544" && R=0 || R=1
ok "client re-tiled to smaller screen (992x544)" test $R -eq 0
kill $WM $XVFB 2>/dev/null; wait 2>/dev/null
echo "$pass PASS, $fail FAIL"
test $fail -eq 0
