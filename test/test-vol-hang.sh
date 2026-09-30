#!/bin/bash
# test-vol-hang.sh - audit-0930 #6: a hung volume backend (wpctl/amixer/
# pactl never returning) must not freeze the event loop.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)
trap 'kill $WM $XVFB 2>/dev/null; pkill -P $$ xterm 2>/dev/null; rm -rf "$H"' EXIT INT TERM
WM=0
mkdir -p "$H/bin"
for b in wpctl amixer pactl; do printf '#!/bin/sh\nsleep 30\n' > "$H/bin/$b"; chmod +x "$H/bin/$b"; done
Xvfb $D -screen 0 1280x800x24 & XVFB=$!
sleep 1
PATH="$H/bin:$PATH" HOME=$H "${WM_BIN:-$TDIR/../daniwm}" & WM=$!
sleep 3
kill -0 $WM 2>/dev/null || { echo "FAIL: wm did not start"; exit 1; }
t0=$(date +%s%N)
xdotool key super+Return
ok=0
for _ in $(seq 1 40); do
    [ "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -ge 1 ] && { ok=1; break; }
    sleep 0.1
done
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
echo "spawn latency: ${ms}ms"
if [ $ok -eq 1 ] && [ $ms -lt 2000 ]; then echo "PASS: key served while vol backend hangs"; echo "RESULT: PASS"; exit 0; fi
echo "FAIL: key served while vol backend hangs"; echo "RESULT: FAIL"; exit 1
