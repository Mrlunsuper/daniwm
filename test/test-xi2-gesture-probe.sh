#!/bin/sh
# Offline only: intentionally never opens a display, even when DISPLAY is set.
set -eu
cd "$(dirname "$0")/.."
p=./test/xi2-gesture-probe
u=./test/xi2-gesture-probe-unsupported
expect_status() {
    expected=$1; shift
    status=0
    output=$("$@" 2>&1) || status=$?
    if [ "$status" -ne "$expected" ]; then
        printf 'FAIL: expected %s got %s: %s\n%s\n' "$expected" "$status" "$*" "$output" >&2
        exit 1
    fi
}
# DISPLAY is a trap: missing explicit display must fail before XOpenDisplay.
export DISPLAY=:98765
expect_status 2 "$p"
expect_status 2 "$p" --mode capabilities
expect_status 2 "$p" --display '' --mode capabilities
expect_status 2 "$p" --display :98765 --mode grab
expect_status 2 "$p" --display :98765 --mode observe --allow-passive-grab
expect_status 2 "$p" --display :98765 --mode capabilities --seconds 1
expect_status 2 "$p" --display :98765 --mode unknown
for duration in 0 61 -1 garbage 1x ''; do
    expect_status 2 "$p" --display :98765 --mode observe --seconds "$duration"
done
expect_status 0 "$p" --build-info
if printf '%s' "$output" | grep -q '^build:'; then
    expect_status 0 "$p" --self-test
    printf '%s\n' "$output" | grep -q 'mask bytes=5 end=32 covered=1'
    printf '%s\n' "$output" | grep -q 'swipe begin .*detail=3'
    printf '%s\n' "$output" | grep -q 'swipe update .*dx=-1.500000 dy=2.500000'
    printf '%s\n' "$output" | grep -q 'swipe end .*cancelled=1'
else
    expect_status 3 "$p" --self-test
    printf '%s\n' "$output" | grep -q '^unsupported:'
fi
expect_status 0 "$u" --build-info
printf '%s\n' "$output" | grep -q '^unsupported:'
expect_status 3 "$u" --display :98765 --mode capabilities
printf '%s\n' "$output" | grep -q 'no connection opened'
expect_status 3 "$u" --self-test
printf '%s\n' 'PASS: offline argument, mask, decoding and unsupported-build checks'
printf '%s\n' 'SKIP: server capabilities, bounded runtime/cleanup, physical delivery and passive grabs require an approved private display/setup'
