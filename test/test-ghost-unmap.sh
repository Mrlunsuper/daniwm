#!/bin/bash
# test-ghost-unmap.sh - T-H1-REP failing repro for plain UnmapNotify ghost.
#
# Problem (H1, audit:65): a managed window that calls plain XUnmapWindow
# stays in `clients` and `_NET_CLIENT_LIST` (list stays 1). Only the
# synthetic-withdraw path (send_event=True, main.c:417-419) unmanages.
#
# XFAIL STATUS: this suite currently asserts the BUGGY result
# (EXPECTED_FINAL=1, ghost present) so `make check` stays green while the
# bug is open. T-H1A flips ONLY the EXPECTED_FINAL line below to 0 --
# no other edit -- and the suite then locks the fixed behavior:
# plain unmap leaves no ghost, WM-hidden windows stay managed.
#
# Scope: test only. No WM code changes. Synthetic-withdraw path untouched.
# Stays on one workspace in tile layout (fresh HOME, no ws keys, no
# monocle toggle) so WM-initiated hides cannot pollute the result.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

# T-H1A: flip to 0 (fixed behavior: plain unmap unmanages, list ends 0).
EXPECTED_FINAL=1

fail=0
assert() { local desc=$1; shift; if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi; }

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

OUT=$(python3 "$TDIR/ghost-unmap.py" 2>"$H/stderr.log")
RC=$?
echo "$OUT"
MANAGED=$(echo "$OUT" | sed -n 's/^MANAGED=//p' | tail -n1)
FINAL=$(echo "$OUT" | sed -n 's/^FINAL=//p' | tail -n1)
[ "$RC" -eq 0 ] || { echo "FAIL: helper setup failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
[ -n "${MANAGED:-}" ] || { echo "FAIL: no MANAGED count from helper"; exit 1; }
[ -n "${FINAL:-}" ] || { echo "FAIL: no FINAL count from helper"; exit 1; }

assert "wm manages mapped window (list 1)" "$MANAGED" -eq 1
if [ "$EXPECTED_FINAL" -eq 1 ]; then
    echo "XFAIL(T-H1-REP): plain client unmap leaves ghost (list stays 1); T-H1A flips EXPECTED_FINAL to 0"
fi
assert "plain unmap leaves _NET_CLIENT_LIST=$EXPECTED_FINAL" "$FINAL" -eq "$EXPECTED_FINAL"

# Manual verification hints (audit H1):
#   DISPLAY=$D xprop -root _NET_CLIENT_LIST
#   DISPLAY=$D xwininfo -root -tree

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
