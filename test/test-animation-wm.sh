#!/bin/sh
# Phase 0 only: isolated semantic smoke baseline with/without compositor.
set -eu
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
for tool in Xvfb setsid timeout; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "SKIP: animation WM baseline needs $tool"; exit 0;
    }
done
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
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/animation-wm.c -o "$tmp/driver" -lX11
$CC $CFLAGS -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror \
    test/animation-settings-xvfb.c -o "$tmp/settings-driver" -lX11
make daniwm dani-comp
mkdir -p "$tmp/home" "$tmp/config/daniwm" "$tmp/bin"
# Explicit bindings isolate the baseline from installed/default key preferences.
printf 'bind = mod+1:ws1\nbind = mod+2:ws2\nbind = mod+o:overview_toggle\n' > "$tmp/config/daniwm/config"
for cmd in dbus-update-activation-environment systemctl; do
    printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$cmd"
    chmod +x "$tmp/bin/$cmd"
done
for mode in instant compositor settings; do
    if [ "$mode" = settings ]; then
        printf 'compositor = 1\nanimations = 1\nreduce_motion = 0\nanimation_open_ms = 1000\nanimation_layout_ms = 160\nbind = mod+Shift+r:reload_config\nbind = mod+c:compositor\n' > "$tmp/config/daniwm/config"
    fi
    : > "$tmp/display"
    Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp -ac \
        3>"$tmp/display" >"$tmp/xvfb.log" 2>&1 &
    xpid=$!
    i=0
    while [ ! -s "$tmp/display" ]; do
        i=$((i + 1))
        if [ "$i" -ge 100 ] || ! kill -0 "$xpid" 2>/dev/null; then
            echo 'FAIL: private Xvfb startup'; grep . "$tmp/xvfb.log" || :; exit 1
        fi
        sleep 0.05
    done
    number=$(tr -d '\n' < "$tmp/display")
    case "$number" in ''|*[!0-9]*) echo 'FAIL: invalid display number'; exit 1;; esac
    env -i PATH="$tmp/bin:$PATH" HOME="$tmp/home" XDG_CONFIG_HOME="$tmp/config" \
        DISPLAY=":$number" setsid ./daniwm >"$tmp/wm.log" 2>&1 &
    wpid=$!
    if [ "$mode" = settings ]; then
        if ! env -i PATH="$PATH" DANI_COMP_NO_PRESENT=1 timeout 30 "$tmp/settings-driver" ":$number" "$tmp/config/daniwm/config"; then
            grep . "$tmp/wm.log" || :; exit 1
        fi
    elif [ "$mode" = compositor ]; then
        if ! env -i PATH="$PATH" timeout 30 "$tmp/driver" ":$number" "$PWD/dani-comp"; then
            grep . "$tmp/wm.log" || :; exit 1
        fi
    else
        if ! env -i PATH="$PATH" timeout 30 "$tmp/driver" ":$number"; then
            grep . "$tmp/wm.log" || :; exit 1
        fi
    fi
    kill -TERM -- "-$wpid" 2>/dev/null || :
    wait "$wpid" 2>/dev/null || :
    wpid=
    kill "$xpid"; wait "$xpid" 2>/dev/null || :
    xpid=
done
