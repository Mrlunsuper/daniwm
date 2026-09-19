#!/usr/bin/env python3
"""T-H1A lock helper: plain client UnmapNotify unmanages, WM hides do not.

Phase 1 maps one window, hides it with plain XUnmapWindow
(send_event=False) and reports _NET_CLIENT_LIST length (FINAL=0 fixed).
Phase 2 storms 20x map/plain-unmap/destroy, reports STORM_FINAL=0.
Phase 3 checks synthetic withdraw still unmanages (SYN_FINAL=0) and a
remap re-manages (REMAP=1).

Stays on one workspace in tile layout; never switches ws, never
toggles monocle, so WM-initiated hides cannot pollute the result.
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


def wait_for(root, atom, want, tries=50, delay=0.1):
    n = -1
    for _ in range(tries):
        n = client_list_len(root, atom)
        if n == want:
            break
        time.sleep(delay)
    return n


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

    win = new_win(d, root, "ghost-unmap-repro")
    win.map()
    d.flush()

    managed = wait_for(root, net_client_list, 1, tries=100)
    print("MANAGED=%d" % managed)
    sys.stdout.flush()
    if managed != 1:
        print("SETUP-FAIL: WM never managed the window", file=sys.stderr)
        try:
            win.destroy()
            d.flush()
        except Exception:
            pass
        return 2

    # Plain client-initiated hide: send_event=False (contrast with the
    # synthetic withdraw path which uses send_event=True).
    win.unmap()
    d.flush()

    final = managed
    for _ in range(8):  # settle ~2s, list must stay stable
        time.sleep(0.25)
        final = client_list_len(root, net_client_list)
    print("FINAL=%d" % final)
    sys.stdout.flush()
    try:
        win.destroy()
        d.flush()
    except Exception:
        pass
    wait_for(root, net_client_list, 0)

    # Storm: 20x map / plain-unmap / destroy, list must end 0.
    storm_final = 0
    for i in range(20):
        w = new_win(d, root, "ghost-storm-%d" % i)
        w.map()
        d.flush()
        if wait_for(root, net_client_list, 1, tries=50) != 1:
            print("STORM-FAIL: manage %d" % i, file=sys.stderr)
            try:
                w.destroy()
                d.flush()
            except Exception:
                pass
            return 2
        w.unmap()
        d.flush()
        time.sleep(0.15)
        try:
            w.destroy()
            d.flush()
        except Exception:
            pass
        storm_final = wait_for(root, net_client_list, 0, tries=50)
        if storm_final != 0:
            break
    print("STORM_FINAL=%d" % storm_final)
    sys.stdout.flush()
    if storm_final != 0:
        d.close()
        return 0  # let shell assert fail with clear output

    # Synthetic withdraw still unmanages, remap re-manages.
    w = new_win(d, root, "ghost-syn-repro")
    w.map()
    d.flush()
    if wait_for(root, net_client_list, 1, tries=100) != 1:
        print("SETUP-FAIL: synthetic window never managed", file=sys.stderr)
        try:
            w.destroy()
            d.flush()
        except Exception:
            pass
        return 2
    w.unmap()
    d.flush()
    ev = event.UnmapNotify(event=root, window=w, from_configure=0)
    root.send_event(
        ev,
        event_mask=X.SubstructureNotifyMask | X.SubstructureRedirectMask,
    )
    d.flush()
    syn_final = wait_for(root, net_client_list, 0, tries=100)
    print("SYN_FINAL=%d" % syn_final)
    w.map()
    d.flush()
    remap = wait_for(root, net_client_list, 1, tries=100)
    print("REMAP=%d" % remap)
    sys.stdout.flush()
    try:
        w.destroy()
        d.flush()
    except Exception:
        pass
    wait_for(root, net_client_list, 0)
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
