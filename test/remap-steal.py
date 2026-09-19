#!/usr/bin/env python3
"""T-H4 helper: known remap must not steal sel/active.

Maps A then B, focuses A via _NET_ACTIVE_WINDOW ClientMessage, then
remaps B with XMapWindow (known-client MapRequest). Reports active
before/after the remap. Fixed: ACTIVE_AFTER stays A in tile and
monocle; new manages still focus.
"""
import os
import sys
import time

from Xlib import X, display
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


def wait_for(root, atom, want, tries=100):
    n = -1
    for _ in range(tries):
        n = client_list_len(root, atom)
        if n == want:
            break
        time.sleep(0.1)
    return n


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

    a = root.create_window(
        10, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
    )
    a.set_wm_name("remap-A")
    a.map()
    d.flush()
    time.sleep(0.4)
    b = root.create_window(
        320, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
    )
    b.set_wm_name("remap-B")
    b.map()
    d.flush()

    if wait_for(root, net_client_list, 2) != 2:
        print("SETUP-FAIL: both windows never managed", file=sys.stderr)
        return 2
    print("A=%d" % a.id)
    print("B=%d" % b.id)

    # Focus A via pager path (B was focused on manage).
    request_active(d, root, net_active, a)
    time.sleep(0.8)
    before = active_window(root, net_active)
    print("ACTIVE_BEFORE=%d" % before)
    if before != a.id:
        print("SETUP-FAIL: could not focus A", file=sys.stderr)
        return 2

    # Remap known B: must not steal.
    b.map()
    d.flush()
    time.sleep(0.8)
    after = active_window(root, net_active)
    print("ACTIVE_AFTER=%d" % after)
    print("REMAP_OK=%d" % (1 if after == a.id else 0))
    sys.stdout.flush()

    for w in (a, b):
        try:
            w.destroy()
        except Exception:
            pass
    d.flush()
    # Wait for cleanup so repeated runs start empty.
    for _ in range(50):
        if client_list_len(root, net_client_list) == 0:
            break
        time.sleep(0.1)
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
