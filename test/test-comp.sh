#!/bin/bash
# test-comp.sh - dani-comp (compositor đơn giản cho daniwm) dưới Xvfb.
# Assert: giữ _NET_WM_CM_Sn, cửa sổ vẫn visible, instance 2 bị từ chối,
# sống sót sau _NET_WM_WINDOW_OPACITY, thoát an toàn, mode --no-shadow chạy.
set -u
TDIR=$(dirname "$0")
# shellcheck source=find_display.sh
. "$TDIR/find_display.sh"
export DISPLAY=$D

fail=0
trap 'kill $WM $COMP $XVFB 2>/dev/null; rm -rf "$CFG"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 +extension COMPOSITE +extension DAMAGE +extension RENDER +extension FIXES &
XVFB=$!
sleep 1
CFG=$(mktemp -d)
HOME=$CFG "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
"${COMP_BIN:-$TDIR/../dani-comp}" --dim 0.9 &
COMP=$!
sleep 1

if kill -0 $COMP 2>/dev/null; then echo "PASS: dani-comp alive";
else echo "FAIL: dani-comp died"; fail=1; fi

for i in 1 2; do xdotool key super+Return; sleep 0.7; done
ok=0; n=0
for _ in $(seq 1 20); do
    n=$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)
    [ "$n" -ge 2 ] && { ok=1; break; }
    sleep 0.5
done
if [ "$ok" = 1 ]; then echo "PASS: 2 xterms visible under compositor";
else echo "FAIL: xterms under compositor (n=$n)"; fail=1; fi

"${COMP_BIN:-$TDIR/../dani-comp}" 2>/tmp/danicomp2.err & C2=$!
sleep 0.5
if kill -0 $C2 2>/dev/null; then echo "FAIL: second instance should exit"; kill $C2; fail=1;
else echo "PASS: second instance refused"; fi

WT=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -1)
if [ -n "$WT" ]; then
    xprop -id "$WT" -f _NET_WM_WINDOW_OPACITY 32c -set _NET_WM_WINDOW_OPACITY 0xcccccccc 2>/dev/null
    sleep 0.8
    if kill -0 $COMP 2>/dev/null; then echo "PASS: survives opacity change";
    else echo "FAIL: died on opacity"; fail=1; fi
fi

# gõ chữ phải hiện qua compositor (có retry: xterm mới spawn shell chưa sẵn)
typed=0
for _ in 1 2 3; do
    WT=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -1)
    [ -n "$WT" ] && xdotool windowfocus "$WT" 2>/dev/null
    sleep 0.5
    import -window root /tmp/danicomp_before.png 2>/dev/null
    xdotool type --delay 10 "DANICOMP" 2>/dev/null
    sleep 1.2
    import -window root /tmp/danicomp_after.png 2>/dev/null
    m1=$(md5sum /tmp/danicomp_before.png 2>/dev/null | cut -d' ' -f1)
    m2=$(md5sum /tmp/danicomp_after.png 2>/dev/null | cut -d' ' -f1)
    if [ -n "$m1" ] && [ "$m1" != "$m2" ]; then typed=1; break; fi
    sleep 1
done
rm -f /tmp/danicomp_before.png /tmp/danicomp_after.png
if [ "$typed" = 1 ]; then echo "PASS: typed text visible through compositor";
else echo "FAIL: typed text not visible"; fail=1; fi

kill $COMP; sleep 0.8
n2=$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)
if [ "$n2" -ge 2 ]; then echo "PASS: windows survive comp exit";
else echo "FAIL: windows lost after comp exit (n=$n2)"; fail=1; fi

"${COMP_BIN:-$TDIR/../dani-comp}" --no-shadow --no-fade --dim 1 &
COMP=$!
sleep 1
if kill -0 $COMP 2>/dev/null; then echo "PASS: no-shadow mode alive";
else echo "FAIL: no-shadow mode died"; fail=1; fi

exit $fail
