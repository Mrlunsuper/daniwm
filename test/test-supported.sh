#!/bin/bash
# test-supported.sh - T-L5: _NET_SUPPORTED matches the honored set.
# Honored atoms present; unimplemented protocols never advertised.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

fail=0

cleanup() {
    kill $WM $XVFB 2>/dev/null
    rm -rf "$H"
}
trap 'cleanup; exit $fail' EXIT INT TERM
WM=0; XVFB=0

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
kill -0 $WM 2>/dev/null || { echo "FAIL: wm did not start"; exit 1; }

SUP=$(xprop -root _NET_SUPPORTED 2>/dev/null)
echo "$SUP" | tr ',' '\n' | head -n 30
[ -n "${SUP:-}" ] || { echo "FAIL: no _NET_SUPPORTED"; exit 1; }

have() { echo "$SUP" | grep -q "$1" && echo "PASS: advertises $1" || { echo "FAIL: missing $1"; fail=1; }; }
absent() { echo "$SUP" | grep -q "$1" && { echo "FAIL: falsely claims $1"; fail=1; } || echo "PASS: omits $1 (unimplemented)"; }

have _NET_WM_WINDOW_TYPE
have _NET_WM_NAME
have _NET_ACTIVE_WINDOW
have _NET_CLOSE_WINDOW
have _NET_WM_STATE
have _NET_WM_STATE_FULLSCREEN
have _NET_CURRENT_DESKTOP
have _NET_WM_DESKTOP
have _NET_WORKAREA
absent _NET_MOVERESIZE_WINDOW
absent _NET_RESTACK_WINDOW
absent _NET_WM_STATE_MAXIMIZED_VERT
absent _NET_WM_STATE_MAXIMIZED_HORZ
absent _NET_SHOWING_DESKTOP

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
