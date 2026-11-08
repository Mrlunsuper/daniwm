#!/bin/bash
# Deterministic flip-buffer lifecycle regression on an isolated Xvfb.
set -eu
TDIR=$(cd "$(dirname "$0")" && pwd)
. "$TDIR/toolchain.sh"
if ! pkg-config --exists xpresent xcomposite xdamage xfixes xrender xext xrandr; then
    echo 'SKIP: compositor Present regression needs XPresent development files'
    exit 0
fi
H=$(mktemp -d)
XVFB=""
trap 'if [ -n "$XVFB" ]; then kill "$XVFB" 2>/dev/null || true; wait "$XVFB" 2>/dev/null || true; fi; rm -rf "$H"' EXIT
$CC $CFLAGS -D_DEFAULT_SOURCE -DWITH_XPRESENT -Wall -Wextra -Wpedantic -Wshadow \
    $(pkg-config --cflags xpresent xcomposite xdamage xfixes xrender xext xrandr) \
    "$TDIR/comp-present.c" -o "$H/comp-present" \
    $(pkg-config --libs xpresent xcomposite xdamage xfixes xrender xext xrandr)
Xvfb -displayfd 3 -screen 0 640x480x24 -nolisten tcp 3>"$H/display" >"$H/xvfb.log" 2>&1 &
XVFB=$!
for _ in $(seq 1 100); do
    [ -s "$H/display" ] && break
    if ! kill -0 "$XVFB" 2>/dev/null; then
        echo 'FAIL: Xvfb exited before display was ready'
        exit 1
    fi
    sleep 0.05
done
if [ ! -s "$H/display" ]; then echo 'FAIL: Xvfb startup timeout'; exit 1; fi
DISPLAY=":$(<"$H/display")" "$H/comp-present"
