#!/bin/sh
# Optional integration: never uses the caller's DISPLAY or session services.
set -eu
if ! command -v Xvfb >/dev/null 2>&1; then
    echo 'SKIP: overview Xvfb integration (Xvfb not installed)'
    exit 0
fi
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
command -v setsid >/dev/null 2>&1 || {
    echo 'SKIP: overview Xvfb integration (setsid not installed)'
    exit 0
}
tmp=$(mktemp -d)
xpid= wpid= driver=
cleanup() {
    trap - EXIT HUP INT TERM
    [ -z "$driver" ] || kill "$driver" 2>/dev/null || :
    # WM helpers inherit its private process group; kill them as well.
    [ -z "$wpid" ] || kill -TERM -- "-$wpid" 2>/dev/null || :
    [ -z "$xpid" ] || kill "$xpid" 2>/dev/null || :
    [ -z "$driver" ] || wait "$driver" 2>/dev/null || :
    [ -z "$wpid" ] || wait "$wpid" 2>/dev/null || :
    [ -z "$xpid" ] || wait "$xpid" 2>/dev/null || :
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
$CC $CFLAGS -std=c11 -Wall -Wextra -Werror test/overview-xvfb.c -o "$tmp/driver" -lX11
make daniwm
mkdir -p "$tmp/home" "$tmp/config/daniwm" "$tmp/bin"
printf 'bind+ = mod+w:expose_toggle\n' > "$tmp/config/daniwm/config"
# session_init forks a detached shell that imports DISPLAY into session services.
# Disable those commands explicitly, in addition to dropping all session env.
for cmd in dbus-update-activation-environment systemctl; do
    printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$cmd"
    chmod +x "$tmp/bin/$cmd"
done
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp -ac \
    3>"$tmp/display" >"$tmp/xvfb.log" 2>&1 &
xpid=$!
i=0
while [ ! -s "$tmp/display" ]; do
    i=$((i + 1))
    if [ "$i" -ge 100 ] || ! kill -0 "$xpid" 2>/dev/null; then
        printf 'FAIL: private Xvfb startup (requires -displayfd)\n' >&2
        grep . "$tmp/xvfb.log" >&2 || :
        exit 1
    fi
    sleep 0.1
done
number=$(tr -d '\n' < "$tmp/display")
case "$number" in ''|*[!0-9]*) echo 'FAIL: invalid private display number' >&2; exit 1;; esac
private_display=:$number
env -i PATH="$tmp/bin:$PATH" HOME="$tmp/home" XDG_CONFIG_HOME="$tmp/config" \
    DISPLAY="$private_display" setsid ./daniwm >"$tmp/wm.log" 2>&1 &
wpid=$!
env -i DISPLAY="$private_display" "$tmp/driver" &
driver=$!
if wait "$driver"; then
    driver=
else
    driver=
    grep . "$tmp/wm.log" >&2 || :
    exit 1
fi
