#!/bin/bash
# measure-h2.sh - A/B đo hiệu quả H2 (damage rect-list clip) so với baseline bbox.
#
# Cùng 1 binary dani-comp: biến môi trường DANI_COMP_BBOX=1 buộc dùng bbox
# (baseline P1 cũ); không set -> dùng clip list rect (H2).
#
# Đo 2 thứ:
#   1. Tỉ lệ diện tích HỢP rect damage / bbox (từ log DANI_COMP_DEBUG=1).
#      < 1 = clip list vẽ ít hơn bbox  -> có lợi.
#      = 1 = XDamage đã gộp 1 hộp      -> clip list vô ích (dùng bbox cho rẻ).
#   2. %CPU dani-comp dưới tải (nhiễu ~±0.1%).
#
# Dùng: test/measure-h2.sh [typing|scroll] [giây]
set -u
TDIR=$(dirname "$0")
# shellcheck source=find_display.sh
. "$TDIR/find_display.sh"
export DISPLAY=$D

LOAD=${1:-typing}   # typing = nhiều rect nhỏ rời; scroll = xterm cuộn liên tục
DUR=${2:-10}

run_one() {
    local mode=$1
    local xlog=$2
    Xvfb "$D" -screen 0 1280x800x24 +extension COMPOSITE +extension DAMAGE \
        +extension RENDER +extension FIXES >/dev/null 2>&1 &
    local xvfb=$!
    sleep 1
    local cfg
    cfg=$(mktemp -d)
    HOME=$cfg "$TDIR/../daniwm" >/dev/null 2>&1 & local wm=$!
    sleep 1
    if [ "$mode" = bbox ]; then
        DANI_COMP_DEBUG=1 DANI_COMP_BBOX=1 "$TDIR/../dani-comp" --dim 0.9 \
            >/dev/null 2>"$xlog" & local comp=$!
    else
        DANI_COMP_DEBUG=1 "$TDIR/../dani-comp" --dim 0.9 \
            >/dev/null 2>"$xlog" & local comp=$!
    fi
    sleep 1

    if [ "$LOAD" = scroll ]; then
        xterm -fa Monospace -fs 12 -geometry 100x40 -e sh -c \
            'while :; do seq 1 6000 | sed "s/^/scroll line long enough to be wide /"; done' \
            >/dev/null 2>&1 & local xt=$!
        sleep 2
    else
        xterm -fa Monospace -fs 12 -geometry 100x40 >/dev/null 2>&1 & local xt=$!
        sleep 2
    fi
    local win
    win=$(xdotool search --onlyvisible --class xterm | head -1)
    xdotool windowactivate "$win" 2>/dev/null
    sleep 0.3

    local c0 c1 t0 t1
    c0=$(awk '{print $14+$15}' "/proc/$comp/stat")
    t0=$(date +%s.%N)
    local end=$(( $(date +%s) + DUR ))
    if [ "$LOAD" = typing ]; then
        while [ "$(date +%s)" -lt "$end" ]; do
            local i
            for i in 1 2 3 4 5 6 7 8 9 10; do
                xdotool type --window "$win" --delay 0 "word$i "
            done
            xdotool key --window "$win" BackSpace BackSpace BackSpace \
                BackSpace BackSpace BackSpace 2>/dev/null
        done
    else
        sleep "$DUR"
    fi
    c1=$(awk '{print $14+$15}' "/proc/$comp/stat")
    t1=$(date +%s.%N)

    kill $xt $comp $wm $xvfb 2>/dev/null
    rm -rf "$cfg"

    local ratio
    ratio=$(grep -oE "bbox=[0-9]+px union=[0-9]+px" "$xlog" | \
        awk -F'[=px ]+' '{b+=$2; u+=$4} END{printf "%.3f", (b?u/b:0)}')
    awk -v m="$mode" -v c0="$c0" -v c1="$c1" -v t0="$t0" -v t1="$t1" -v r="$ratio" \
        -v ld="$LOAD" \
        'BEGIN{printf "load=%-6s mode=%-4s union/bbox=%-6s avg_cpu=%.2f%% (ticks=%d wall=%.2fs)\n", \
         ld, m, r, (c1-c0)/100/(t1-t0)*100, c1-c0, t1-t0}'
}

echo "### load=$LOAD dur=${DUR}s"
run_one bbox /tmp/h2-bbox.log
run_one list /tmp/h2-list.log
