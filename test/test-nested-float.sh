#!/bin/bash
# test-nested-float.sh - small floating window nested inside a big one must
# stay reachable via hover (sloppy focus). Before the fix, crossing the big
# window auto-raised it over the small one, so the pointer could never
# reach the small window (focus stuck on big).
# Usage: ./test-nested-float.sh (needs Xvfb, xterm, xdotool)
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
export DISPLAY=$D
H=$(mktemp -d)

fail=0
assert() {
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"; exit $fail' EXIT INT TERM

# stacking helper: prints xterm window ids, bottom -> top (XQueryTree order)
HELPER=$H/stackorder
cat > "$H/stackorder.c" <<'EOF'
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Window r = DefaultRootWindow(d), p, *k = NULL;
    unsigned n = 0;
    if (XQueryTree(d, r, &r, &p, &k, &n) && k) {
        for (unsigned i = 0; i < n; i++) {
            XClassHint ch = {0};
            if (XGetClassHint(d, k[i], &ch)) {
                int isx = (ch.res_class && !strcmp(ch.res_class, "XTerm"));
                if (ch.res_class) XFree(ch.res_class);
                if (ch.res_name) XFree(ch.res_name);
                if (isx) printf("%lu\n", (unsigned long)k[i]);
            }
        }
        XFree(k);
    }
    XCloseDisplay(d);
    return 0;
}
EOF
cc -O2 -o "$HELPER" "$H/stackorder.c" -lX11 || { echo "FAIL: cannot build helper"; exit 1; }
topmost() { "$HELPER" 2>/dev/null | tail -n1; }
# ground truth window under the pointer (XQueryPointer, not geometry math)
ptrwin() { xdotool getmouselocation 2>/dev/null | sed -n 's/.*window:\([0-9]*\).*/\1/p'; }
geom() { xdotool getwindowgeometry --shell "$1" 2>/dev/null | tr '\n' ' '; }

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

xdotool key super+Return; sleep 1
xdotool key super+Return; sleep 1
i=0; while [ "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -lt 2 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
[ "$(xdotool search --onlyvisible --class xterm 2>/dev/null | wc -l)" -ge 2 ] || { echo "FAIL: no 2 xterms"; exit 1; }
w1=$(xdotool search --onlyvisible --class xterm 2>/dev/null | head -n1)
w2=$(xdotool search --onlyvisible --class xterm 2>/dev/null | tail -n1)

# float w1 as the big window: hover it first (sloppy focus), then float,
# then place it. (Hover-then-act: deterministic under sloppy focus.)
xdotool mousemove 320 400; sleep 0.5
xdotool key super+Shift+space; sleep 0.5
big=$(xdotool getwindowfocus 2>/dev/null)
xdotool windowmove "$big" 100 100 windowsize "$big" 600 500; sleep 0.5

# float the other one as the small window: hover a big-free area first.
small=$(if [ "$big" = "$w1" ]; then echo "$w2"; else echo "$w1"; fi)
xdotool mousemove 1200 700; sleep 0.5
xdotool key super+Shift+space; sleep 0.5
# whatever got floated here is our small window (focus follows pointer)
small=$(xdotool getwindowfocus 2>/dev/null)
xdotool windowmove "$small" 300 300 windowsize "$small" 200 150; sleep 0.5
# big id = the other one
big=$(if [ "$small" = "$w1" ]; then echo "$w2"; else echo "$w1"; fi)
echo "INFO big=$big small=$small"

# ensure small on top (client raise requests are honored for floating)
xdotool windowraise "$small"; sleep 0.8
assert "small starts on top" "$(topmost)" = "$small"

# hover: from a big-only area into the small center, step by step
# (the pointer really crosses big first, like a human would)
xdotool mousemove 150 150; sleep 0.5
for p in "200 200" "250 250" "300 300" "350 340" "400 375"; do
    set -- $p
    xdotool mousemove "$1" "$2"; sleep 0.3
done
sleep 0.8
focus=$(xdotool getwindowfocus 2>/dev/null)
assert "hover reaches nested small window (focus=$focus small=$small)" "$focus" = "$small"

# hover must not have restacked big over small
assert "hover does not raise big over small (top=$(topmost))" "$(topmost)" = "$small"

# Mod+click raises explicitly (grab path): press with Super, no motion.
# Plain clicks go straight to the app and must never restack (otherwise
# the hover trap from the original bug would come back through clicks).
# Settle the pointer first so no stale motion is queued when the grab
# fires (stale motion would drag the window on press).
xdotool mousemove 150 150; sleep 0.5
xdotool mousemove 150 150; sleep 0.5
assert "pointer settled on big" "$(ptrwin)" = "$big"
biggeom0=$(geom "$big")
# separate down/up with settles (like test-mouse.sh drag): back-to-back
# XTEST press+release can lose the release across the grab, leaving a
# stuck drag session that a later release would end spuriously.
xdotool keydown super; sleep 0.3
xdotool mousedown 1; sleep 0.5
xdotool mouseup 1; sleep 0.3
xdotool keyup super; sleep 0.8
assert "mod+click focuses big (focus=$(xdotool getwindowfocus))" "$(xdotool getwindowfocus 2>/dev/null)" = "$big"
assert "mod+click raises big (top=$(topmost))" "$(topmost)" = "$big"
assert "mod+click without motion does not move big" "$(geom "$big")" = "$biggeom0"

# keyboard rescue: small is fully covered now; Super+j/k must still
# reach it (explicit focus raises) — no window is ever unreachable.
rescued=""
for i in 1 2 3 4; do
    xdotool key super+j; sleep 0.5
    if [ "$(xdotool getwindowfocus 2>/dev/null)" = "$small" ]; then rescued=1; break; fi
done
assert "keyboard focus reaches covered small" "$rescued" = 1
assert "keyboard focus raises small (top=$(topmost))" "$(topmost)" = "$small"

# plain click on the small window: focuses (via hover) without restacking.
# Precondition (ground truth): pointer really is over small and small is
# on top — otherwise the test setup itself raced, not the WM.
xdotool mousemove 400 375; sleep 0.5
xdotool mousemove 400 375; sleep 0.5
assert "pointer settled on small" "$(ptrwin)" = "$small"
assert "small on top before click" "$(topmost)" = "$small"
xdotool click 1; sleep 0.8
assert "click keeps small focused" "$(xdotool getwindowfocus 2>/dev/null)" = "$small"
assert "plain click never restacks" "$(topmost)" = "$small"

exit $fail
