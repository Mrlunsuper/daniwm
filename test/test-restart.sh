#!/bin/bash
# test-restart.sh - restart-in-place keeps clients (ws, focus, desktop,
# parked scratchpad). Spawns 2 xterms + parks a scratchpad, restarts via
# Super+Ctrl+r, asserts everything survives with workspaces intact.
# Restart is detected via _DANIWM_HEARTBEAT (PID is kept across exec and
# the X server may recycle window IDs, so neither proves a restart).
set -u
TDIR=$(dirname "$0")
# shellcheck source=find_display.sh
. "$TDIR/find_display.sh"   # sets D to a free display number
export DISPLAY=$D

fail=0
assert() { # assert <desc> <test...>
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc";
    else echo "FAIL: $desc"; fail=1; fi
}
nvis() { xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l; }
nall() { xdotool search --class xterm 2>/dev/null | wc -l; }
ns() { xdotool search --onlyvisible --classname scratchpad 2>/dev/null | wc -l; }
wait_vis() { # wait_vis <count> <timeout_s>
    local want=$1 timeout=$2 i=0
    while [ $i -lt $(( timeout * 2 )) ]; do
        [ "$(nvis)" -eq "$want" ] && return 0
        sleep 0.5; i=$(( i + 1 ))
    done
    return 1
}
wait_scratch() { # wait for scratchpad visible
    local i=0
    while [ "$(ns)" -eq 0 ] && [ $i -lt 80 ]; do sleep 0.5; i=$(( i + 1 )); done
    [ "$(ns)" -ge 1 ]
}
beat() { xprop -root _DANIWM_HEARTBEAT 2>/dev/null | awk '{print $NF}'; }
client_count() { xprop -root _NET_CLIENT_LIST 2>/dev/null | tr ',' '\n' | grep -c '0x'; }
settle() { sleep 1; }

do_restart() { # do_restart <desc> — poll heartbeat for the re-exec
    local desc=$1 before after swapped=0
    before=$(beat)
    xdotool key super+ctrl+r
    for _ in $(seq 1 20); do
        sleep 0.5
        after=$(beat)
        if [ -n "$after" ] && [ "$after" != "$before" ]; then swapped=1; break; fi
    done
    assert "$desc" "$swapped" -eq 1
}

trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
# Hermetic HOME so ambient dev config never leaks in (cf. F01).
H=$(mktemp -d)
unset XDG_CONFIG_HOME
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

for i in 1 2; do xdotool key super+Return; sleep 0.5; done
wait_vis 2 60 || { echo "FAIL: 2 xterms never appeared"; exit 1; }
echo "PASS: 2 tiled windows visible"

# Park the focused window on ws2 (send_to follows), back to ws1.
xdotool key super+shift+2; settle
assert "moved to ws2 (1 visible)" "$(nvis)" -eq 1
xdotool key super+1; settle
assert "back on ws1 (1 visible)" "$(nvis)" -eq 1
assert "2 clients tracked" "$(client_count)" -eq 2

# Spawn + park the scratchpad (hidden, sticky, filtered from client list).
xdotool key super+s
wait_scratch || { echo "FAIL: scratchpad never spawned"; exit 1; }
echo "PASS: scratchpad spawned"
xdotool key super+s; settle
assert "scratchpad parked" "$(ns)" -eq 0

do_restart "restart happened (heartbeat swapped)"

wait_vis 1 60 || { echo "FAIL: ws1 window never reappeared after restart"; exit 1; }
assert "all windows survived restart (2 terms + parked scratch)" "$(nall)" -eq 3
assert "client list intact after restart" "$(client_count)" -eq 2
assert "still on ws1 (1 visible)" "$(nvis)" -eq 1
assert "parked scratchpad stayed hidden" "$(ns)" -eq 0

# The ws2 window kept its workspace across the restart.
xdotool key super+2; settle
assert "ws2 window restored to ws2 (1 visible)" "$(nvis)" -eq 1
xdotool key super+1; settle
assert "ws1 window still on ws1 (1 visible)" "$(nvis)" -eq 1

# The parked scratchpad unparks after restart.
xdotool key super+s; settle
assert "parked scratchpad reshown after restart" "$(ns)" -ge 1

# The new instance serves keys: spawn works post-restart.
xdotool key super+Return; sleep 1
assert "spawn works after restart" "$(nall)" -ge 3

if [ "$fail" -ne 0 ]; then echo "RESULT: FAIL"; else echo "RESULT: PASS"; fi
exit $fail
