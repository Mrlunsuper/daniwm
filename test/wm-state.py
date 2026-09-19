#!/usr/bin/env python3
"""T-M4A check helper: ICCCM WM_STATE NormalState/WithdrawnState mirror.

Maps one window, waits for manage, reads WM_STATE (expect 1 =
NormalState). Then performs a synthetic withdraw (plain unmap +
synthetic UnmapNotify with send_event=True, the ICCCM withdraw path),
waits for unmanage (_NET_CLIENT_LIST back to 0), and reads WM_STATE
again (expect 0 = WithdrawnState).

Prints MANAGED_STATE=<n>, UNMANAGED=<n>, WITHDRAWN_STATE=<n>.
Exit 0 on a clean run, 2 on setup failure.
"""
import os
import sys
import time

from Xlib import X, display
from Xlib.protocol import event


def prop_list_len(root, atom):
    prop = root.get_full_property(atom, X.AnyPropertyType)
    if prop is None or prop.value is None:
        return 0
    return len(prop.value)


def wm_state(win, atom):
    prop = win.get_full_property(atom, X.AnyPropertyType)
    if prop is None or not prop.value:
        return -1
    return int(prop.value[0])


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    wm_state_atom = d.intern_atom("WM_STATE")

    win = root.create_window(
        10, 10, 200, 150, 0,
        d.screen().root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=0xFFFFFF,
        event_mask=X.StructureNotifyMask,
    )
    win.set_wm_name("wm-state-repro")
    win.map()
    d.flush()

    n = 0
    for _ in range(100):  # up to ~10s for manage
        n = prop_list_len(root, net_client_list)
        if n == 1:
            break
        time.sleep(0.1)
    if n != 1:
        print("SETUP-FAIL: WM never managed the window", file=sys.stderr)
        try:
            win.destroy()
            d.flush()
        except Exception:
            pass
        return 2

    managed_state = wm_state(win, wm_state_atom)
    print("MANAGED_STATE=%d" % managed_state)
    sys.stdout.flush()

    # Synthetic ICCCM withdraw: real unmap + synthetic UnmapNotify
    # (send_event=True) so the WM takes the unmanage path.
    win.unmap()
    d.flush()
    ev = event.UnmapNotify(event=root, window=win, from_configure=0)
    root.send_event(
        ev,
        event_mask=X.SubstructureNotifyMask | X.SubstructureRedirectMask,
    )
    d.flush()

    n = 1
    for _ in range(100):  # up to ~10s for unmanage
        n = prop_list_len(root, net_client_list)
        if n == 0:
            break
        time.sleep(0.1)
    print("UNMANAGED=%d" % n)
    withdrawn_state = wm_state(win, wm_state_atom)
    print("WITHDRAWN_STATE=%d" % withdrawn_state)
    sys.stdout.flush()

    try:
        win.destroy()
        d.flush()
    except Exception:
        pass
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
