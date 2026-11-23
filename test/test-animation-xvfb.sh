#!/bin/sh
# Phase 0 only. Compile even without Xvfb; never use the caller's DISPLAY.
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
libs='x11 xcomposite xdamage xfixes xrender xext'
# Every fixture includes comp.c, including the production detached-image owner.
CFLAGS="$CFLAGS src/exit-layer.c"
pkg-config --exists $libs || {
    echo 'SKIP: animation prototype needs X11 compositor development libraries'
    exit 0
}
res_flags=
if pkg-config --exists xcb-res; then
    libs="$libs xcb-res xcb"
    res_flags=-DWITH_XCB_RES
fi
$CC $CFLAGS $res_flags -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    $(pkg-config --cflags $libs) test/comp-animation.c test/animation-snapshot.c src/animation.c \
    -o "$tmp/prototype" $(pkg-config --libs $libs)
$CC $CFLAGS -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    $(pkg-config --cflags $libs) test/comp-animation-loop.c src/animation.c \
    -o "$tmp/loop" $(pkg-config --libs $libs)
$CC $CFLAGS -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    $(pkg-config --cflags $libs) test/comp-fade.c src/animation.c \
    -o "$tmp/fade" $(pkg-config --libs $libs)
$CC $CFLAGS -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    $res_flags $(pkg-config --cflags $libs) test/comp-close.c src/animation.c \
    -o "$tmp/close" $(pkg-config --libs $libs)
echo 'PASS: Phase 0/1A/1B X11 tests compile warning-free (copy fallback)'
env -u DISPLAY -u WAYLAND_DISPLAY "$tmp/prototype" --self-test
if pkg-config --exists xpresent xrandr; then
    $CC $CFLAGS $res_flags -D_DEFAULT_SOURCE -DWITH_XPRESENT -std=c11 \
        -Wall -Wextra -Wpedantic -Wshadow -Werror \
        $(pkg-config --cflags $libs xpresent xrandr) \
        test/comp-animation.c test/animation-snapshot.c src/animation.c \
        -o "$tmp/prototype-present" $(pkg-config --libs $libs xpresent xrandr)
    echo 'PASS: Phase 0 X11 prototype compiles warning-free (Present-enabled; runtime uses copy)'
    env -u DISPLAY -u WAYLAND_DISPLAY "$tmp/prototype-present" --self-test
    $CC $CFLAGS -D_DEFAULT_SOURCE -DWITH_XPRESENT -std=c11 \
        -Wall -Wextra -Wpedantic -Wshadow -Werror \
        $(pkg-config --cflags $libs xpresent xrandr) test/comp-animation-loop.c src/animation.c \
        -o "$tmp/loop-present" $(pkg-config --libs $libs xpresent xrandr)
else
    echo 'SKIP: Present-enabled prototype build needs XPresent development files'
fi
if ! command -v Xvfb >/dev/null 2>&1; then
    echo 'SKIP: Phase 0 snapshot/baseline runtime BLOCKED (Xvfb not installed)'
    exit 0
fi
command -v timeout >/dev/null 2>&1 || {
    echo 'SKIP: Phase 0 private-display runtime needs timeout'
    exit 0
}
Xvfb -displayfd 3 -screen 0 640x480x24 -nolisten tcp -ac \
    3>"$tmp/display" >"$tmp/xvfb.log" 2>&1 &
xpid=$!
i=0
while [ ! -s "$tmp/display" ]; do
    i=$((i + 1))
    if [ "$i" -ge 100 ] || ! kill -0 "$xpid" 2>/dev/null; then
        echo 'FAIL: private Xvfb startup (requires -displayfd)' >&2
        grep . "$tmp/xvfb.log" >&2 || :
        exit 1
    fi
    sleep 0.05
done
number=$(tr -d '\n' < "$tmp/display")
case "$number" in ''|*[!0-9]*) echo 'FAIL: invalid private display number' >&2; exit 1;; esac
# Drop session environment. The helper also requires an explicit display arg.
env -i PATH="$PATH" timeout 30 "$tmp/close" ":$number"
env -i PATH="$PATH" timeout 30 "$tmp/fade" ":$number"
env -i PATH="$PATH" timeout 30 "$tmp/prototype" ":$number"
if [ -x "$tmp/prototype-present" ]; then
    env -i PATH="$PATH" timeout 30 "$tmp/prototype-present" ":$number"
fi
for mode in fade instant zero reduced cancel close close-cancel remap burst; do
    env -i PATH="$PATH" timeout 10 "$tmp/loop" ":$number" "$mode" copy
    if [ -x "$tmp/loop-present" ]; then
        env -i PATH="$PATH" timeout 10 "$tmp/loop-present" ":$number" "$mode" present
    fi
done
