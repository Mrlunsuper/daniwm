#!/bin/bash
# test-ghost-unmap.sh - T-H1A lock: plain client UnmapNotify unmanages.
#
# Fixed behavior (was T-H1-REP XFAIL): a managed window that calls plain
# XUnmapWindow leaves `clients` and `_NET_CLIENT_LIST` empty (list 0).
# WM-initiated hides (monocle-hidden, ws switch) stay managed by design.
#
# Stays on one workspace in tile layout (fresh HOME, no ws keys, no
# monocle toggle) so WM-initiated hides cannot pollute the result.
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

# Fixed behavior: plain unmap unmanages, list ends 0.
EXPECTED_FINAL=0

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
STORM=$(echo "$OUT" | sed -n 's/^STORM_FINAL=//p' | tail -n1)
SYN=$(echo "$OUT" | sed -n 's/^SYN_FINAL=//p' | tail -n1)
REMAP=$(echo "$OUT" | sed -n 's/^REMAP=//p' | tail -n1)
[ "$RC" -eq 0 ] || { echo "FAIL: helper setup failed (rc=$RC)"; cat "$H/stderr.log"; exit 1; }
[ -n "${MANAGED:-}" ] || { echo "FAIL: no MANAGED count from helper"; exit 1; }
[ -n "${FINAL:-}" ] || { echo "FAIL: no FINAL count from helper"; exit 1; }
[ -n "${STORM:-}" ] || { echo "FAIL: no STORM_FINAL count from helper"; exit 1; }
[ -n "${SYN:-}" ] || { echo "FAIL: no SYN_FINAL count from helper"; exit 1; }
[ -n "${REMAP:-}" ] || { echo "FAIL: no REMAP count from helper"; exit 1; }

assert "wm manages mapped window (list 1)" "$MANAGED" -eq 1
assert "plain unmap leaves _NET_CLIENT_LIST=$EXPECTED_FINAL" "$FINAL" -eq "$EXPECTED_FINAL"
assert "20x map/plain-unmap storm ends 0" "$STORM" -eq 0
assert "synthetic withdraw still unmanages (list 0)" "$SYN" -eq 0
assert "remap after withdraw re-manages (list 1)" "$REMAP" -eq 1

# Manual verification hints (audit H1):
#   DISPLAY=$D xprop -root _NET_CLIENT_LIST
#   DISPLAY=$D xwininfo -root -tree

[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
