#!/bin/sh
# Headless Phase 0 ownership tests; never opens DISPLAY.
set -eu
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/test-animation.c test/animation-snapshot.c -o "$tmp/animation"
"$tmp/animation"
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/test-animation-open.c src/animation.c -o "$tmp/open"
"$tmp/open"
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/test-animation-close.c src/exit-layer.c src/animation.c -o "$tmp/close"
"$tmp/close"
$CC $CFLAGS -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Werror \
    $(pkg-config --cflags xft) -ffunction-sections -fdata-sections \
    test/test-config-animation.c src/state.c -Wl,--gc-sections -lm -o "$tmp/config"
"$tmp/config"
