#!/bin/bash
# test-click-raise-float.sh - plain click on a floating window behind
# another floating window must (a) raise it to the front, (b) mark it
# active (_NET_ACTIVE_WINDOW), and (c) still deliver the click (press AND
# release) to the app. Before the fix the WM was blind to plain clicks on
# floats: the back window never came forward and never looked active.
# Usage: ./test-click-raise-float.sh (needs Xvfb, xdotool)
set -u
TDIR=$(dirname "$0")
. "$TDIR/find_display.sh"
. "$TDIR/toolchain.sh"
export DISPLAY=$D
H=$(mktemp -d)

fail=0
assert() {
    local desc=$1; shift
    if [ "$@" ]; then echo "PASS: $desc"; else echo "FAIL: $desc"; fail=1; fi
}
trap 'kill $WM $XVFB 2>/dev/null; rm -rf "$H"' EXIT INT TERM

# stacking helper: lists FloatClick/XTerm ids bottom -> top
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
                int hit = ch.res_class && (!strcmp(ch.res_class, "XTerm") ||
                                           !strcmp(ch.res_class, "FloatClick"));
                if (ch.res_class) XFree(ch.res_class);
                if (ch.res_name) XFree(ch.res_name);
                if (hit) printf("%lu\n", (unsigned long)k[i]);
            }
        }
        XFree(k);
    }
    XCloseDisplay(d);
    return 0;
}
EOF
${CC} ${CFLAGS} -o "$HELPER" "$H/stackorder.c" -lX11 || { echo "FAIL: cannot build helper"; exit 1; }
topmost() { "$HELPER" 2>/dev/null | tail -n1; }
activewin() { xprop -root _NET_ACTIVE_WINDOW 2>/dev/null | sed -n 's/.*window id # //p'; }
actdec() { local v; v=$(activewin); [ -n "$v" ] && echo $((v)); }
presses() { grep -c "^PRESS" "$1" 2>/dev/null; }
presses() { grep -c "^PRESS" "$1" 2>/dev/null || true; }
releases() { grep -c "^RELEASE" "$1" 2>/dev/null || true; }

"$CC" ${CFLAGS} -o "$H/float-click-helper" "$TDIR/float-click-helper.c" -lX11 || { echo "FAIL: cannot build click helper"; exit 1; }

Xvfb $D -screen 0 1280x800x24 &
XVFB=$!
sleep 1
HOME=$H "${WM_BIN:-$TDIR/../daniwm}" &
WM=$!
sleep 1

# two floating helper windows, back = a, front = b (overlapping)
"$H/float-click-helper" > "$H/a.log" 2>&1 &
sleep 0.5
i=0; while ! grep -q READY "$H/a.log" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
grep -q READY "$H/a.log" || { echo "FAIL: helper a never ready"; exit 1; }
a=$(awk '/READY/{print $2}' "$H/a.log" | tail -n1)

xdotool mousemove 320 400; sleep 0.5
xdotool key super+Shift+space; sleep 0.5   # float a
xdotool windowmove "$a" 100 100
xdotool windowsize "$a" 500 400; sleep 0.5

"$H/float-click-helper" > "$H/b.log" 2>&1 &
sleep 0.5
i=0; while ! grep -q READY "$H/b.log" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
grep -q READY "$H/b.log" || { echo "FAIL: helper b never ready"; exit 1; }
b=$(awk '/READY/{print $2}' "$H/b.log" | tail -n1)

xdotool mousemove 1200 700; sleep 0.5
xdotool key super+Shift+space; sleep 0.5   # float b
xdotool windowmove "$b" 300 250
xdotool windowsize "$b" 500 400; sleep 0.5
xdotool windowraise "$b"; sleep 0.8

echo "INFO back(a)=$a front(b)=$b top=$(topmost)"
assert "b on top before click" "$(topmost)" = "$b"

# hover into a's exposed region (focus no-raise), then PLAIN click
xdotool mousemove 330 270; sleep 0.5    # still on b (overlap = b owns it)
xdotool mousemove 150 150; sleep 0.8    # a-only region
focus0=$(xdotool getwindowfocus 2>/dev/null)
assert "hover focuses back window first" "$focus0" = "$a"
assert "hover does not raise (back still below)" "$(topmost)" = "$b"

na=$(presses "$H/a.log")
xdotool click 1; sleep 1.0
assert "plain click raises back window to front" "$(topmost)" = "$a"
assert "plain click marks back window active" "$(actdec)" = "$a"
assert "app received the press" "$(presses "$H/a.log")" -gt "$na"
assert "app received the release" "$(releases "$H/a.log")" -gt 0

# keyboard focus back to b (raise), then plain click on b
xdotool key super+k; sleep 0.8
assert "keyboard raises the other float" "$(topmost)" = "$b"
nb=$(presses "$H/b.log")
xdotool mousemove 400 350; sleep 0.8    # b region (a covers it too but b on top)
xdotool click 1; sleep 1.0
assert "plain click on front window keeps it on top" "$(topmost)" = "$b"
assert "front window app received the press" "$(presses "$H/b.log")" -gt "$nb"

# Shift+click (text-selection flow) must still reach the app ungrabbed
ns=$(presses "$H/b.log")
xdotool keydown Shift; sleep 0.2
xdotool click 1; sleep 0.8
xdotool keyup Shift; sleep 0.3
assert "Shift+click still reaches the app" "$(presses "$H/b.log")" -gt "$ns"

# Mod+click must still focus + raise (grab path, no replay)
xdotool keydown super; sleep 0.3
xdotool mousedown 1; sleep 0.5
xdotool mouseup 1; sleep 0.3
xdotool keyup super; sleep 0.8
assert "Mod+click still focuses" "$(xdotool getwindowfocus 2>/dev/null)" = "$b"
assert "Mod+click still raises" "$(topmost)" = "$b"

exit $fail