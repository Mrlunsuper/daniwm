#!/usr/bin/env python3
"""T-H1-REP repro helper: plain client UnmapNotify ghost.

Maps one normal window, waits for the WM to manage it
(_NET_CLIENT_LIST length 1), then hides it with a plain
XUnmapWindow (send_event=False, client-initiated hide -- NOT the
synthetic withdraw path) and reports _NET_CLIENT_LIST length.

Expected before T-H1A (bug): FINAL=1 (ghost stays listed).
Expected after T-H1A (fix):  FINAL=0 (unmanage on plain unmap).

Stays on one workspace in tile layout; never switches ws, never
toggles monocle, so the unmap cannot be mistaken for a WM-initiated
hide (ws switch / monocle-hidden windows stay managed by design).
"""
import os
import sys
import time

from Xlib import X, display


def client_list_len(root, atom):
    prop = root.get_full_property(atom, X.AnyPropertyType)
    if prop is None or prop.value is None:
        return 0
    return len(prop.value)


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")

    win = root.create_window(
        10, 10, 200, 150, 0,
        d.screen().root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=0xFFFFFF,
        event_mask=X.StructureNotifyMask,
    )
    win.set_wm_name("ghost-unmap-repro")
    win.map()
    d.flush()

    managed = 0
    for _ in range(100):  # up to ~10s for manage
        managed = client_list_len(root, net_client_list)
        if managed == 1:
            break
        time.sleep(0.1)
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

    final = 1
    for _ in range(8):  # settle ~2s, list must stay stable
        time.sleep(0.25)
        final = client_list_len(root, net_client_list)
    print("FINAL=%d" % final)
    sys.stdout.flush()

    # Cleanup: destroy so the (possibly ghost) client is released via
    # DestroyNotify; keeps repeated runs independent.
    try:
        win.destroy()
        d.flush()
    except Exception:
        pass
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
