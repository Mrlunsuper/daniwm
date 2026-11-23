#!/bin/sh
# Phase 2 gate experiment only. Never inherit or guess a test DISPLAY.
set -eu
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
tmp=$(mktemp -d)
xpid=
cleanup() {
    trap - EXIT HUP INT TERM
    [ -z "$xpid" ] || kill "$xpid" 2>/dev/null || :
    [ -z "$xpid" ] || wait "$xpid" 2>/dev/null || :
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/test-layout-protocol.c -o "$tmp/protocol"
env -u DISPLAY -u WAYLAND_DISPLAY "$tmp/protocol"
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/test-layout-eligibility.c -o "$tmp/eligibility"
env -u DISPLAY -u WAYLAND_DISPLAY "$tmp/eligibility"
[ "${1:-}" != --headless ] || exit 0
libs='x11 xi xcomposite xdamage xfixes xrender xext'
if ! pkg-config --exists $libs; then
    echo 'BLOCKED: layout prototype needs X11 compositor/XI2 development libraries'
    exit 0
fi
res_flags=
if pkg-config --exists xcb-res; then
    libs="$libs xcb-res xcb"
    res_flags=-DWITH_XCB_RES
else
    echo 'SKIP: XRes measurement needs xcb-res development files'
fi
$CC $CFLAGS $res_flags -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    $(pkg-config --cflags $libs) test/layout-prototype-xvfb.c src/animation.c src/exit-layer.c \
    -o "$tmp/prototype" $(pkg-config --libs $libs) -ldl
if ! command -v Xvfb >/dev/null 2>&1 || ! command -v timeout >/dev/null 2>&1; then
    echo 'BLOCKED: layout prototype runtime needs Xvfb and timeout'
    exit 0
fi
Xvfb -displayfd 3 -screen 0 640x480x24 -nolisten tcp -ac \
    3>"$tmp/display" >"$tmp/xvfb.log" 2>&1 &
xpid=$!
i=0
while [ ! -s "$tmp/display" ]; do
    i=$((i+1))
    if [ "$i" -ge 100 ] || ! kill -0 "$xpid" 2>/dev/null; then
        echo 'FAIL: private Xvfb startup' >&2
        grep . "$tmp/xvfb.log" >&2 || :
        exit 1
    fi
    sleep 0.05
done
number=$(tr -d '\n' < "$tmp/display")
case "$number" in ''|*[!0-9]*) echo 'FAIL: invalid private display number' >&2; exit 1;; esac
env -i PATH="$PATH" timeout -k 3 30 "$tmp/prototype" ":$number"
