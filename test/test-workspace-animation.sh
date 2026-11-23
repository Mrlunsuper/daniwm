#!/bin/sh
# Phase 3-I workspace crossfade: real daniwm + WM-managed dani-comp on a private Xvfb.
# Runs once with the copy path and once with Present.
set -eu
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
for tool in Xvfb setsid timeout pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "SKIP: workspace animation check needs $tool"; exit 0;
    }
done
if ! pkg-config --exists x11-xcb xcb-res; then
    echo 'SKIP: workspace animation check needs x11-xcb and xcb-res'; exit 0
fi
tmp=$(mktemp -d)
xpid= wpid=
stop() {
    [ -z "$wpid" ] || kill -TERM -- "-$wpid" 2>/dev/null || :
    [ -z "$xpid" ] || kill "$xpid" 2>/dev/null || :
    [ -z "$wpid" ] || wait "$wpid" 2>/dev/null || :
    [ -z "$xpid" ] || wait "$xpid" 2>/dev/null || :
    xpid= wpid=
}
cleanup() {
    trap - EXIT HUP INT TERM
    stop
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
# shellcheck disable=SC2046
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/workspace-animation-xvfb.c -o "$tmp/driver" \
    $(pkg-config --cflags --libs x11 x11-xcb xcb-res) -ldl
make daniwm dani-comp
mkdir -p "$tmp/home" "$tmp/config/daniwm" "$tmp/bin"
for cmd in dbus-update-activation-environment systemctl; do
    printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$cmd"
    chmod +x "$tmp/bin/$cmd"
done
cfg="$tmp/config/daniwm/config"
run() {
    mode=$1
    printf 'compositor = 1\nanimations = 1\nreduce_motion = 0\nanimation_open_ms = 0\nanimation_close_ms = 0\nanimation_layout_ms = 0\nanimation_workspace_ms = 250\nbar_on = 0\ngaps_on = 0\nborder = 0\nfocus = click\nbind = mod+Shift+r:reload_config\nbind = mod+m:monocle\nbind = mod+t:tile\nbind = mod+o:overview_toggle\n' > "$cfg"
    rm -f "$tmp/display"
    Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp \
        +extension COMPOSITE +extension DAMAGE +extension RENDER +extension XFIXES \
        3>"$tmp/display" >"$tmp/xvfb.log" 2>&1 &
    xpid=$!
    i=0
    while [ ! -s "$tmp/display" ]; do
        i=$((i + 1))
        if [ "$i" -ge 100 ] || ! kill -0 "$xpid" 2>/dev/null; then
            echo 'FAIL: private Xvfb startup'; exit 1
        fi
        sleep 0.05
    done
    number=$(tr -d '\n' < "$tmp/display")
    case "$number" in ''|*[!0-9]*) echo 'FAIL: invalid display number'; exit 1;; esac
    if [ "$mode" = copy ]; then present=DANI_COMP_NO_PRESENT=1; else present=DANI_COMP_PRESENT_TEST=1; fi
    env -i PATH="$tmp/bin:$PATH" HOME="$tmp/home" XDG_CONFIG_HOME="$tmp/config" \
        "$present" ${DANI_COMP_DEBUG:+DANI_COMP_DEBUG=$DANI_COMP_DEBUG} DISPLAY=":$number" setsid ./daniwm >"$tmp/wm-$mode.log" 2>&1 &
    wpid=$!
    if ! env -i PATH="$PATH" timeout 90 "$tmp/driver" ":$number" "$cfg" "$mode"; then
        grep . "$tmp/wm-$mode.log" || :
        exit 1
    fi
    stop
    if grep 'dani-comp: Xerror' "$tmp/wm-$mode.log"; then
        echo "FAIL: dani-comp X errors ($mode)"; exit 1
    fi
    echo "PASS: no unexpected dani-comp X errors ($mode)"
}
run copy
run present
echo 'PASS: Phase 3-I workspace crossfade checks'
