#!/bin/bash
# measure-arrange.sh - audit-0930 #7: ConfigureRequest storm cost (not in
# run-all; prints drain time + WM CPU ticks).
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)
trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"' EXIT INT TERM
Xvfb $D -screen 0 1280x800x24 & XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" 2>/dev/null & WM=$!
sleep 1
cpu() { awk '{print $14+$15}' /proc/$WM/stat; }
c0=$(cpu)
python3 "$TDIR/cfg-spam.py" "${1:-2000}"
echo "wm_cpu_ticks=$(( $(cpu) - c0 ))"
