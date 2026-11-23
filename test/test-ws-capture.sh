#!/bin/sh
# Phase 3-P1 workspace capture prototype. Test-only; never linked into binaries.
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
    test/test-ws-capture-protocol.c -o "$tmp/protocol"
env -u DISPLAY -u WAYLAND_DISPLAY "$tmp/protocol"
[ "${1:-}" != --headless ] || exit 0
libs='x11 xcomposite x11-xcb xcb-res'
if ! pkg-config --exists $libs; then
    echo 'BLOCKED: 3-P1 prototype needs x11, xcomposite, x11-xcb and xcb-res development files'
    exit 0
fi
for tool in Xvfb timeout; do
    command -v "$tool" >/dev/null 2>&1 || { echo "BLOCKED: 3-P1 prototype needs $tool"; exit 0; }
done
# shellcheck disable=SC2046
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    $(pkg-config --cflags $libs) test/ws-capture-xvfb.c -o "$tmp/prototype" \
    $(pkg-config --libs $libs)
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp \
    +extension COMPOSITE +extension DAMAGE +extension RENDER +extension XFIXES \
    3>"$tmp/display" >"$tmp/xvfb.log" 2>&1 &
xpid=$!
i=0
while [ ! -s "$tmp/display" ]; do
    i=$((i + 1))
    if [ "$i" -ge 100 ] || ! kill -0 "$xpid" 2>/dev/null; then
        echo 'FAIL: private Xvfb startup'; exit 1
    fi
    sleep 0.05
done
number=$(tr -d '\n' < "$tmp/display")
case "$number" in ''|*[!0-9]*) echo 'FAIL: invalid display number'; exit 1;; esac
env -i PATH="$PATH" timeout 120 "$tmp/prototype" drive ":$number"
