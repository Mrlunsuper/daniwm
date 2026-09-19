#!/usr/bin/env python3
"""T-M3 helper: workspace switch storm keeps the client list stable.

Creates 10 windows on ws0, then flips view between ws1 and ws0 fifty
times via _NET_CURRENT_DESKTOP ClientMessage. Reports list length and
whether the WM survived. Unmap-first ordering means no transient
double-map and no ghosts (H1A ignores the ws-mismatched unmaps).
"""
import os
import sys
import time

from Xlib import X, display
from Xlib.protocol import event


def client_list_len(root, atom):
    prop = root.get_full_property(atom, X.AnyPropertyType)
    if prop is None or prop.value is None:
        return 0
    return len(prop.value)


def wait_for(root, atom, want, tries=100):
    for _ in range(tries):
        if client_list_len(root, atom) == want:
            return want
        time.sleep(0.1)
    return client_list_len(root, atom)


def goto_ws(d, root, atom, n):
    ev = event.ClientMessage(
        window=root, client_type=atom, format=32,
        data=(32, [n, 0, 0, 0, 0]),
    )
    root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    d.flush()


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    cur_desktop = d.intern_atom("_NET_CURRENT_DESKTOP")

    wins = []
    for i in range(10):
        w = root.create_window(
            10 + i * 5, 10, 200, 150, 0, d.screen().root_depth,
            X.InputOutput, X.CopyFromParent,
            background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
        )
        w.set_wm_name("ws-storm-%d" % i)
        w.map()
        d.flush()
        wins.append(w)
    if wait_for(root, net_client_list, 10) != 10:
        print("SETUP-FAIL: 10 windows never managed", file=sys.stderr)
        return 2
    print("MANAGED=10")

    for i in range(50):
        goto_ws(d, root, cur_desktop, 1 if i % 2 == 0 else 0)
        time.sleep(0.05)
    goto_ws(d, root, cur_desktop, 0)
    time.sleep(1.0)

    n = client_list_len(root, net_client_list)
    print("STORM_LIST=%d" % n)
    print("STORM_OK=%d" % (1 if n == 10 else 0))
    sys.stdout.flush()

    for w in wins:
        try:
            w.destroy()
        except Exception:
            pass
    d.flush()
    wait_for(root, net_client_list, 0)
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
