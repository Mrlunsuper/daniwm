#!/bin/bash
# test-config.sh - config file: scalars, binds, bad lines, reload.
# Usage: ./test-config.sh (needs Xvfb, xterm, xdotool)
set -u
D=:98
export DISPLAY=$D
TDIR=$(dirname "$0")
H=$(mktemp -d)
mkdir -p "$H/.config/daniwm"
cat > "$H/.config/daniwm/config" <<'EOF'
gaps_on = 0
mfact = 0.8
bogus_key = 123
bind = mod+Return:spawn_term
bind = mod+Shift+r:reload_config
rule = noon:*:bogus:*
bind = mod+q:does_not_exist
EOF

fail=0
assert() { # assert <desc> <test...>
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
geom() { # geom <winid> -> "x w"
    xdotool getwindowgeometry --shell "$1" 2>/dev/null | awk -F= '/^X=/{x=$2} /^WIDTH=/{w=$2} END{print x+0, w+0}'
}

trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

# custom-kept spawn bind works despite bad lines elsewhere
for i in 1 2 3; do xdotool key super+Return; sleep 0.5; done
i=0; while [ "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -lt 3 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
assert "config boot: 3 visible" "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -eq 3

# gaps_on=0 -> master at x=0; mfact=0.8 -> master w=1020
read -r mx mw <<< "$(geom "$(xdotool search --class xterm 2>/dev/null | head -n1)")"
# master is the widest window; find max width instead of assuming order
mx=0; mw=0
for id in $(xdotool search --class xterm 2>/dev/null); do
    read -r x w <<< "$(geom "$id")"
    if [ "$w" -gt "$mw" ]; then mw=$w; mx=$x; fi
done
assert "config gaps_off master x" "$mx" -eq 0
assert "config mfact master w" "$mw" -eq 1020

# reload minimal config -> defaults back (gaps on, outer gap x=14)
cat > "$H/.config/daniwm/config" <<'EOF'
gaps_on = 1
gap_outer = 10
EOF
xdotool key super+Shift+r; sleep 1.5
mx=0; mw=0
for id in $(xdotool search --class xterm 2>/dev/null); do
    read -r x w <<< "$(geom "$id")"
    if [ "$w" -gt "$mw" ]; then mw=$w; mx=$x; fi
done
assert "reload restores outer gap" "$mx" -eq 14

exit $fail
