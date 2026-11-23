#!/bin/bash
# test-comp.sh - dani-comp (compositor đơn giản cho daniwm) dưới Xvfb.
# Assert: giữ _NET_WM_CM_Sn, cửa sổ vẫn visible, instance 2 bị từ chối,
# sống sót sau _NET_WM_WINDOW_OPACITY, thoát an toàn, mode --no-shadow chạy.
set -u
TDIR=$(cd "$(dirname "$0")" && pwd)
for tool in Xvfb setsid xterm xdotool xprop import md5sum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: compositor regression needs $tool"
        exit 0
    fi
done
fail=0
WM= COMP= XVFB= C2=
CFG=$(mktemp -d)
cleanup() {
    trap - EXIT INT TERM
    [ -z "$C2" ] || kill "$C2" 2>/dev/null || :
    [ -z "$COMP" ] || kill "$COMP" 2>/dev/null || :
    [ -z "$WM" ] || kill -TERM -- "-$WM" 2>/dev/null || :
    [ -z "$XVFB" ] || kill "$XVFB" 2>/dev/null || :
    [ -z "$C2" ] || wait "$C2" 2>/dev/null || :
    [ -z "$COMP" ] || wait "$COMP" 2>/dev/null || :
    [ -z "$WM" ] || wait "$WM" 2>/dev/null || :
    [ -z "$XVFB" ] || wait "$XVFB" 2>/dev/null || :
    rm -rf "$CFG"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
# Never fall back to a guessed or inherited display after server failure.
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp \
    +extension COMPOSITE +extension DAMAGE +extension RENDER +extension XFIXES \
    3>"$CFG/display" >"$CFG/xvfb.log" 2>&1 &
XVFB=$!
for _ in $(seq 1 100); do
    [ -s "$CFG/display" ] && break
    if ! kill -0 "$XVFB" 2>/dev/null; then
        echo 'FAIL: private Xvfb startup'; exit 1
    fi
    sleep 0.05
done
D=$(tr -d '\n' < "$CFG/display")
case "$D" in ''|*[!0-9]*) echo 'FAIL: private Xvfb display unavailable'; exit 1;; esac
export DISPLAY=:$D
# Phase 1B CLI validation must fail before claiming compositor ownership.
for duration in -1 1001 100ms 999999999999999999999; do
    if "${COMP_BIN:-$TDIR/../dani-comp}" --close-ms "$duration" >"$CFG/cli.log" 2>&1; then
        echo "FAIL: invalid close duration accepted: $duration"; fail=1
    fi
done
if [ "$fail" = 0 ]; then echo 'PASS: close duration rejects invalid/overflow input'; fi
mkdir -p "$CFG/bin" "$CFG/config/daniwm"
: > "$CFG/config/daniwm/config"
# Prevent session_init from importing a private DISPLAY into user services.
for cmd in dbus-update-activation-environment systemctl; do
    printf '#!/bin/sh\nexit 0\n' > "$CFG/bin/$cmd"
    chmod +x "$CFG/bin/$cmd"
done
env -i PATH="$CFG/bin:$PATH" HOME="$CFG" XDG_CONFIG_HOME="$CFG/config" \
    DISPLAY="$DISPLAY" setsid "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1
"${COMP_BIN:-$TDIR/../dani-comp}" --dim 0.9 &
COMP=$!
sleep 1

if kill -0 $COMP 2>/dev/null; then echo "PASS: dani-comp alive";
else echo "FAIL: dani-comp died"; fail=1; fi

for i in 1 2; do xdotool key super+Return; sleep 0.7; done
ok=0; n=0
for _ in $(seq 1 20); do
    n=$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)
    [ "$n" -ge 2 ] && { ok=1; break; }
    sleep 0.5
done
if [ "$ok" = 1 ]; then echo "PASS: 2 xterms visible under compositor";
else echo "FAIL: xterms under compositor (n=$n)"; fail=1; fi

"${COMP_BIN:-$TDIR/../dani-comp}" 2>"$CFG/second-comp.err" & C2=$!
sleep 0.5
if kill -0 $C2 2>/dev/null; then echo "FAIL: second instance should exit"; kill $C2; fail=1;
else echo "PASS: second instance refused"; fi
wait "$C2" 2>/dev/null || :
C2=

WT=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -1)
if [ -n "$WT" ]; then
    xprop -id "$WT" -f _NET_WM_WINDOW_OPACITY 32c -set _NET_WM_WINDOW_OPACITY 0xcccccccc 2>/dev/null
    sleep 0.8
    if kill -0 $COMP 2>/dev/null; then echo "PASS: survives opacity change";
    else echo "FAIL: died on opacity"; fail=1; fi
fi

# gõ chữ phải hiện qua compositor (có retry: xterm mới spawn shell chưa sẵn)
typed=0
for _ in 1 2 3; do
    WT=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -1)
    [ -n "$WT" ] && xdotool windowfocus "$WT" 2>/dev/null
    sleep 0.5
    import -window root "$CFG/before.png" 2>/dev/null
    xdotool type --delay 10 "DANICOMP" 2>/dev/null
    sleep 1.2
    import -window root "$CFG/after.png" 2>/dev/null
    m1=$(md5sum "$CFG/before.png" 2>/dev/null | cut -d' ' -f1)
    m2=$(md5sum "$CFG/after.png" 2>/dev/null | cut -d' ' -f1)
    if [ -n "$m1" ] && [ "$m1" != "$m2" ]; then typed=1; break; fi
    sleep 1
done
rm -f "$CFG/before.png" "$CFG/after.png"
if [ "$typed" = 1 ]; then echo "PASS: typed text visible through compositor";
else echo "FAIL: typed text not visible"; fail=1; fi

kill "$COMP"; wait "$COMP" 2>/dev/null || :
COMP=
sleep 0.8
n2=$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)
if [ "$n2" -ge 2 ]; then echo "PASS: windows survive comp exit";
else echo "FAIL: windows lost after comp exit (n=$n2)"; fail=1; fi

"${COMP_BIN:-$TDIR/../dani-comp}" --no-shadow --no-fade --close-ms 0 --dim 1 &
COMP=$!
sleep 1
if kill -0 $COMP 2>/dev/null; then echo "PASS: no-shadow mode alive";
else echo "FAIL: no-shadow mode died"; fail=1; fi

exit $fail
