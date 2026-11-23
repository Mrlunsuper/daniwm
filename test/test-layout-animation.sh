#!/bin/sh
# Phase 2 best-effort layout movement: WM + WM-managed dani-comp on a private Xvfb.
set -eu
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
for tool in Xvfb setsid timeout pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "SKIP: layout animation check needs $tool"; exit 0;
    }
done
if ! pkg-config --exists x11-xcb xcb-res; then
    echo 'SKIP: layout animation check needs x11-xcb and xcb-res'; exit 0
fi
tmp=$(mktemp -d)
xpid= wpid=
cleanup() {
    trap - EXIT HUP INT TERM
    [ -z "$wpid" ] || kill -TERM -- "-$wpid" 2>/dev/null || :
    [ -z "$xpid" ] || kill "$xpid" 2>/dev/null || :
    [ -z "$wpid" ] || wait "$wpid" 2>/dev/null || :
    [ -z "$xpid" ] || wait "$xpid" 2>/dev/null || :
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
# shellcheck disable=SC2046
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/layout-animation-xvfb.c -o "$tmp/driver" \
    $(pkg-config --cflags --libs x11 x11-xcb xcb-res)
make daniwm dani-comp
mkdir -p "$tmp/home" "$tmp/config/daniwm" "$tmp/bin"
for cmd in dbus-update-activation-environment systemctl; do
    printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$cmd"
    chmod +x "$tmp/bin/$cmd"
done
cfg="$tmp/config/daniwm/config"
printf 'compositor = 1\nanimations = 1\nreduce_motion = 0\nanimation_open_ms = 0\nanimation_close_ms = 0\nanimation_layout_ms = 1000\nnmaster = 2\nbar_on = 0\ngaps_on = 0\nborder = 0\nfocus = click\nbind = mod+Shift+r:reload_config\nbind = mod+k:move_up\nbind = mod+j:move_down\n' > "$cfg"
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
env -i PATH="$tmp/bin:$PATH" HOME="$tmp/home" XDG_CONFIG_HOME="$tmp/config" \
    DANI_COMP_NO_PRESENT=1 DISPLAY=":$number" setsid ./daniwm >"$tmp/wm.log" 2>&1 &
wpid=$!
if ! env -i PATH="$PATH" timeout 60 "$tmp/driver" ":$number" "$cfg"; then
    grep . "$tmp/wm.log" || :
    exit 1
fi
echo 'PASS: Phase 2 best-effort layout movement checks'
