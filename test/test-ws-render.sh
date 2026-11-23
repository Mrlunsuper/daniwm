#!/bin/sh
# Phase 3-P2 workspace crossfade prototype. Test-only; never linked into binaries.
set -eu
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
libs='x11 xcomposite xdamage xfixes xrender xrandr xext x11-xcb xcb-res'
if ! pkg-config --exists $libs; then
    echo "BLOCKED: 3-P2 prototype needs development files for: $libs"
    exit 0
fi
for tool in Xvfb timeout; do
    command -v "$tool" >/dev/null 2>&1 || { echo "BLOCKED: 3-P2 prototype needs $tool"; exit 0; }
done
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
# shellcheck disable=SC2046
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    $(pkg-config --cflags $libs) test/ws-render-xvfb.c -o "$tmp/prototype" \
    $(pkg-config --libs $libs) -ldl
Xvfb -displayfd 3 -screen 0 1600x700x24 -nolisten tcp \
    +extension COMPOSITE +extension DAMAGE +extension RENDER +extension XFIXES +extension RANDR \
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
env -i PATH="$PATH" timeout 150 "$tmp/prototype" drive ":$number"
