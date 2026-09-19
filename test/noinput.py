#!/usr/bin/env python3
"""T-H2.2 helper: WM_HINTS input=False never gets XSetInputFocus.

Maps normal N and NoInput U (WM_HINTS InputHint input=0). Focuses U
via _NET_ACTIVE_WINDOW ClientMessage (pager path). Expects sel/active
to track U but real input focus to stay off U. Then focuses N and
expects both active and real focus on N.

Prints NOINPUT_ACTIVE=<id> NOINPUT_FOCUS=<id> NOINPUT_OK=0/1
NORMAL_ACTIVE=<id> NORMAL_FOCUS=<id> NORMAL_OK=0/1.
"""
import os
import sys
import time

from Xlib import X, display, Xutil
from Xlib.protocol import event


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


def wait_for(root, atom, want, tries=50):
    n = -1
    for _ in range(tries):
        n = client_list_len(root, atom)
        if n == want:
            break
        time.sleep(0.1)
    return n


def get_focus(d):
    try:
        f = d.get_input_focus().focus
        return f.id if hasattr(f, "id") else int(f)
    except Exception:
        return 0


def request_active(d, root, net_active, win):
    ev = event.ClientMessage(
        window=win,
        client_type=net_active,
        format=32,
        data=(32, [1, 0, 0, 0, 0]),
    )
    root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    d.flush()


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    net_active = d.intern_atom("_NET_ACTIVE_WINDOW")

    n = root.create_window(
        10, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
    )
    n.set_wm_name("noinput-normal")
    n.set_wm_hints(flags=Xutil.InputHint, input=1)
    n.map()
    d.flush()
    time.sleep(0.4)

    u = root.create_window(
        320, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0xFF0000, event_mask=X.StructureNotifyMask,
    )
    u.set_wm_name("noinput-refuser")
    u.set_wm_hints(flags=Xutil.InputHint, input=0)
    u.map()
    d.flush()

    if wait_for(root, net_client_list, 2, tries=100) != 2:
        print("SETUP-FAIL: WM did not manage both windows", file=sys.stderr)
        return 2
    time.sleep(0.6)
    print("N=%d" % n.id)
    print("U=%d" % u.id)
    sys.stdout.flush()

    # Focus the NoInput window via pager path.
    request_active(d, root, net_active, u)
    time.sleep(0.8)
    na = active_window(root, net_active)
    nf = get_focus(d)
    print("NOINPUT_ACTIVE=%d" % na)
    print("NOINPUT_FOCUS=%d" % nf)
    noinput_ok = 1 if (na == u.id and nf != u.id) else 0
    print("NOINPUT_OK=%d" % noinput_ok)
    sys.stdout.flush()

    # Normal window still takes real input.
    request_active(d, root, net_active, n)
    time.sleep(0.8)
    ra = active_window(root, net_active)
    rf = get_focus(d)
    print("NORMAL_ACTIVE=%d" % ra)
    print("NORMAL_FOCUS=%d" % rf)
    normal_ok = 1 if (ra == n.id and rf == n.id) else 0
    print("NORMAL_OK=%d" % normal_ok)
    sys.stdout.flush()

    for w in (n, u):
        try:
            w.destroy()
        except Exception:
            pass
    d.flush()
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
