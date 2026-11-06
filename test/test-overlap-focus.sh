#!/bin/bash
# Regression: monocle restacking must not generate an endless hover focus loop.
set -euo pipefail
TDIR=$(cd "$(dirname "$0")" && pwd)
. "$TDIR/find_display.sh"
. "$TDIR/toolchain.sh"
export DISPLAY=$D
H=$(mktemp -d)
WM= XVFB=
cleanup() {
    [ -z "$WM" ] || kill "$WM" 2>/dev/null || true
    [ -z "$XVFB" ] || kill "$XVFB" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$H"
}
trap cleanup EXIT
${CC} ${CFLAGS} -Wall -Wextra -o "$H/overlap-focus" "$TDIR/overlap-focus.c" -lX11
Xvfb "$D" -screen 0 1280x800x24 >"$H/xvfb.log" 2>&1 &
XVFB=$!
sleep 1
mkdir -p "$H/.config/daniwm"
fail=0
for layout in tile monocle; do
    printf 'focus = hover\nrule = OverlapFocus:*:float:1:%s\n' "$layout" >"$H/.config/daniwm/config"
    HOME=$H XDG_CONFIG_HOME=$H/.config "${WM_BIN:-$TDIR/../daniwm}" >"$H/wm.log" 2>&1 &
    WM=$!
    sleep 1
    kill -0 "$WM"
    echo "=== overlapping floats: $layout ==="
    if ! "$H/overlap-focus"; then
        echo "FAIL: overlapping floats in $layout"
        fail=1
    fi
    kill "$WM"
    wait "$WM" || true
    WM=
done
if [ "$fail" -eq 0 ]; then echo 'RESULT: PASS'; else echo 'RESULT: FAIL'; fi
exit "$fail"
