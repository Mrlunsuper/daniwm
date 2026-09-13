#!/bin/bash
# verify.sh - automated tilewm verification under Xvfb.
# Tools: Xvfb, xterm, xdotool, import. No wmctrl (its -lG misreports here).
# Usage: ./verify.sh
set -u
TDIR=$(dirname "$0")
# shellcheck source=find_display.sh
. "$TDIR/find_display.sh"   # sets D to a free display number
export DISPLAY=$D

SHOT=$TDIR/shots
mkdir -p "$SHOT"
rm -f "$SHOT"/*.png

fail=0
assert() { # assert <desc> <test...>
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc";
    else echo "FAIL: $desc"; fail=1; fi
}
nvis() { xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l; }
wait_vis() { # wait_vis <count> <timeout_s>
    local want=$1 timeout=$2 i=0
    while [ $i -lt $(( timeout * 2 )) ]; do
        [ "$(nvis)" -eq "$want" ] && return 0
        sleep 0.5; i=$(( i + 1 ))
    done
    return 1
}
xywh() { # xywh <winid> -> "x y w h"
    xdotool getwindowgeometry --shell "$1" 2>/dev/null | awk -F= '/^X=/{x=$2} /^Y=/{y=$2} /^WIDTH=/{w=$2} /^HEIGHT=/{h=$2} END{print x+0, y+0, w+0, h+0}'
}
shot() { import -window root "$SHOT/$1.png"; }
settle() { sleep 1; }

trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$CFG"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
CFG=$(mktemp -d)
HOME=$CFG "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

for i in 1 2 3; do xdotool key super+Return; sleep 0.5; done
wait_vis 3 60 || { echo "FAIL: 3 xterms never appeared"; shot 00-nospawn; exit 1; }
echo "PASS: 3 tiled windows visible"
master=0; stack=0
for id in $(xdotool search --class xterm 2>/dev/null); do
    read -r x y w h <<< "$(xywh "$id")"
    echo "tile win: x=$x y=$y w=$w h=$h"
    if [ "$x" -ge 10 -a "$x" -le 20 -a "$w" -ge 670 -a "$w" -le 690 ]; then
        master=$((master+1))
        if [ "$y" -lt 30 -o "$y" -gt 45 ]; then echo "FAIL: master y=$y outside bar+gap band"; fail=1; fi
    fi
    if [ "$x" -ge 700 -a "$x" -le 715 ]; then
        stack=$((stack+1))
        if [ "$y" -lt 30 ]; then echo "FAIL: stack y=$y above bar"; fail=1; fi
    fi
done
assert "one master" "$master" -eq 1
assert "two stack" "$stack" -eq 2

# monocle: exactly 1 visible, fills area with gap_inner (14,38,1248x744)
xdotool key super+m; settle
assert "monocle shows 1" "$(nvis)" -eq 1
read -r xm ym wm hm <<< "$(xywh "$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -n1)")"
echo "monocle: x=$xm y=$ym w=$wm h=$hm"
assert "monocle geometry" "$xm" -eq 14 -a "$ym" -eq 38 -a "$wm" -eq 1248 -a "$hm" -eq 744
shot 02-monocle

# back to tile; gaps off -> master x~0; gaps on restores
xdotool key super+space; settle
xdotool key super+g; settle
gapoff=1
for id in $(xdotool search --class xterm 2>/dev/null); do
    read -r x y w h <<< "$(xywh "$id")"
    if [ "$x" -ge 10 -a "$x" -le 20 ]; then gapoff=0; fi
done
assert "gaps-off removes outer gap" "$gapoff" -eq 1
shot 03-nogap
xdotool key super+g; settle

# workspace: ws2 empty, ws1 back to 3
xdotool key super+2; settle
assert "ws2 empty" "$(nvis)" -eq 0
shot 04-ws2empty
xdotool key super+1; settle
assert "back to ws1, 3 visible" "$(nvis)" -eq 3

# move to ws2 (follow): ws2 shows 1; ws1 back to 2
xdotool key super+Shift+2; settle
assert "after move, ws2 shows 1" "$(nvis)" -eq 1
shot 05-moved
xdotool key super+1; settle
assert "ws1 back to 2" "$(nvis)" -eq 2

# fullscreen covers whole monitor (0,0,1280x800, over bar)
xdotool key super+2; settle
xdotool key super+f; settle
read -r xf yf wf hf <<< "$(xywh "$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -n1)")"
echo "fullscreen: x=$xf y=$yf w=$wf h=$hf"
assert "fullscreen geometry" "$xf" -eq 0 -a "$yf" -eq 0 -a "$wf" -eq 1280 -a "$hf" -eq 800
shot 06-fullscreen
xdotool key super+f; settle

# scratchpad spawn / hide / reshow
ns() { xdotool search --onlyvisible --classname scratchpad 2>/dev/null | wc -l; }
xdotool key super+s
i=0; while [ "$(ns)" -eq 0 ] && [ $i -lt 80 ]; do sleep 0.5; i=$((i+1)); done
assert "scratchpad spawned" "$(ns)" -ge 1
shot 07-scratch
xdotool key super+s; settle
assert "scratchpad hidden" "$(ns)" -eq 0
# EWMH: parked scratchpad must be filtered from _NET_CLIENT_LIST
scratch_all=$(xdotool search --classname scratchpad 2>/dev/null | head -n1 || true)
if [ -n "${scratch_all:-}" ]; then
    scratch_hex=$(printf "0x%x" "$scratch_all")
    cl_hide=$(xprop -root _NET_CLIENT_LIST 2>/dev/null || true)
    echo "client_list while parked: $cl_hide"
    if echo "$cl_hide" | grep -qi "${scratch_hex#0x}"; then echo "FAIL: parked scratchpad leaked into _NET_CLIENT_LIST"; fail=1; else echo "PASS: parked scratchpad filtered from _NET_CLIENT_LIST"; fi
else
    echo "FAIL: scratchpad window id not found for EWMH check"; fail=1
fi
xdotool key super+s; settle
assert "scratchpad reshown" "$(ns)" -ge 1
# EWMH: reshown scratchpad must re-appear in _NET_CLIENT_LIST
if [ -n "${scratch_all:-}" ]; then
    cl_show=$(xprop -root _NET_CLIENT_LIST 2>/dev/null || true)
    echo "client_list while shown: $cl_show"
    if echo "$cl_show" | grep -qi "${scratch_hex#0x}"; then echo "PASS: reshown scratchpad in _NET_CLIENT_LIST"; else echo "FAIL: reshown scratchpad missing from _NET_CLIENT_LIST"; fail=1; fi
fi

echo "shots in $SHOT"

# EWMH: supporting window carries _NET_WM_NAME + _NET_WM_PID == WM pid
cw=$(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | awk '{print $NF}')
wmpid=$(xprop -id "$cw" _NET_WM_PID 2>/dev/null | awk -F' = ' '{print $2}')
if [ "$wmpid" = "$WM" ]; then echo "PASS: _NET_WM_PID $wmpid == WM pid";
else echo "FAIL: _NET_WM_PID $wmpid != WM pid $WM"; fail=1; fi
if xprop -root _NET_SUPPORTED 2>/dev/null | grep -q _NET_WM_PID; then echo "PASS: _NET_WM_PID advertised";
else echo "FAIL: _NET_WM_PID not in _NET_SUPPORTED"; fail=1; fi

exit $fail
