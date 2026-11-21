#!/bin/sh
# Offline only: never opens a display.
set -eu
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
flags="$(pkg-config --cflags xft) -Isrc -std=c11 -Wall -Wextra -Werror"
$CC $CFLAGS $flags test/test-swipe.c src/swipe.c -lm -o "$tmp/swipe"
"$tmp/swipe"
$CC $CFLAGS $flags test/test-gestures.c src/gestures.c src/swipe.c -lm -o "$tmp/adapter"
"$tmp/adapter"
$CC $CFLAGS $flags -DDANIWM_NO_GESTURES test/test-gestures.c src/gestures.c src/swipe.c -lm -o "$tmp/disabled"
"$tmp/disabled"
$CC $CFLAGS $flags -ffunction-sections -fdata-sections test/test-config-swipe.c src/state.c -Wl,--gc-sections -lm -o "$tmp/config"
"$tmp/config"
