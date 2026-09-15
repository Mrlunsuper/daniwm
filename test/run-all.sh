#!/bin/sh
# run-all.sh - run every daniwm headless suite under Xvfb, aggregate results.
# Usage: ./test/run-all.sh  (or: make check)
set -u
TDIR=$(dirname "$0")
fail=0
for t in verify test-config test-kill test-mouse test-workspaces test-strut test-randr test-tray test-restart test-tasklist test-comp test-nested-float test-click-delivery; do
    echo "=== $t ==="
    out=$(mktemp)
    "$TDIR/$t.sh" >"$out" 2>&1
    rc=$?
    grep -E "PASS|FAIL|ok:|RESULT" "$out" | grep -vE "xkbcomp" || true
    rm -f "$out"
    if [ $rc -ne 0 ]; then echo "FAIL (suite): $t"; fail=1; else echo "ok: $t"; fi
done
if [ $fail -ne 0 ]; then echo "RESULT: FAIL"; else echo "RESULT: ALL PASS"; fi
exit $fail
