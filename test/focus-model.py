#!/usr/bin/env python3
"""T-H2.0 harness helper: observe focus desync between WM sel and X focus.

Maps windows A then B (WM focuses B on manage). Records
_NET_ACTIVE_WINDOW, then forces real input focus to A with
XSetInputFocus and re-reads _NET_ACTIVE_WINDOW plus XGetInputFocus.

Prints A=<id> B=<id> ACTIVE=<id> FOCUS=<id> SYNC=0/1.
SYNC=1 when WM active window equals real input focus.
Pre T-H2.1 (bug): SYNC=0 (WM still points at B, real focus is A).
Post T-H2.1 (fix): SYNC=1.

Uses XSetInputFocus directly, not _NET_ACTIVE_WINDOW ClientMessage,
so the focus-fight guard (ClientMessage-only) never triggers.
"""
import os
import sys
import time

from Xlib import X, display


def active_window(root, atom):
    prop = root.get_full_property(atom, X.AnyPropertyType)
    if prop is None or not prop.value:
        return 0
    return int(prop.value[0])


def client_list_len(root, atom):
    prop = root.get_full_property(atom, X.AnyPropertyType)
    if prop is None or prop.value is None:
        return 0
    return len(prop.value)


def new_win(d, root, name):
    win = root.create_window(
        10, 10, 200, 150, 0,
        d.screen().root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=0xFFFFFF,
        event_mask=X.StructureNotifyMask,
    )
    win.set_wm_name(name)
    return win


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    net_active = d.intern_atom("_NET_ACTIVE_WINDOW")

    a = new_win(d, root, "focus-harness-A")
    a.map()
    d.flush()
    time.sleep(0.6)
    b = new_win(d, root, "focus-harness-B")
    b.map()
    d.flush()

    n = 0
    for _ in range(100):
        n = client_list_len(root, net_client_list)
        if n == 2:
            break
        time.sleep(0.1)
    if n != 2:
        print("SETUP-FAIL: WM did not manage both windows", file=sys.stderr)
        for w in (a, b):
            try:
                w.destroy()
            except Exception:
                pass
        d.flush()
        return 2

    time.sleep(0.8)  # let manage focus settle on B
    print("A=%d" % a.id)
    print("B=%d" % b.id)
    print("ACTIVE_BEFORE=%d" % active_window(root, net_active))
    sys.stdout.flush()

    # Force real focus to A from outside the WM.
    try:
        a.set_input_focus(X.RevertToPointerRoot, X.CurrentTime)
    except Exception as e:
        print("SETUP-FAIL: XSetInputFocus: %s" % e, file=sys.stderr)
        return 2
    d.flush()
    time.sleep(0.8)  # let FocusIn propagate (or not, pre-fix)

    active = active_window(root, net_active)
    focus = d.get_input_focus().focus.id if hasattr(d.get_input_focus().focus, "id") else 0
    # get_input_focus returns (focus, revert); focus may be a Window or id.
    try:
        f = d.get_input_focus().focus
        focus = f.id if hasattr(f, "id") else int(f)
    except Exception:
        focus = 0
    print("ACTIVE=%d" % active)
    print("FOCUS=%d" % focus)
    sync = 1 if (active == focus and active == a.id) else 0
    print("SYNC=%d" % sync)
    sys.stdout.flush()

    # Storm: 50 forced focus flips A/B, no crash, active tracks last.
    last = b
    storm_ok = 1
    try:
        for i in range(50):
            last = a if (i % 2 == 0) else b
            last.set_input_focus(X.RevertToPointerRoot, X.CurrentTime)
            d.flush()
            time.sleep(0.05)
    except Exception as e:
        print("STORM-FAIL: %s" % e, file=sys.stderr)
        storm_ok = 0
    d.flush()
    time.sleep(0.5)
    s_active = active_window(root, net_active)
    try:
        f = d.get_input_focus().focus
        s_focus = f.id if hasattr(f, "id") else int(f)
    except Exception:
        s_focus = 0
    print("STORM_ACTIVE=%d" % s_active)
    print("STORM_FOCUS=%d" % s_focus)
    print("STORM_LAST=%d" % last.id)
    if storm_ok and s_active == last.id and s_focus == last.id:
        print("STORM_SYNC=1")
    else:
        print("STORM_SYNC=0")
    sys.stdout.flush()

    for w in (a, b):
        try:
            w.destroy()
        except Exception:
            pass
    d.flush()
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
