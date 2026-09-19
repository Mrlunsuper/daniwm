#!/usr/bin/env python3
"""T-M5B helper: event-driven kills carry a real timestamp.

Maps a WM_DELETE-protocol window, finds the bar, then sweeps middle
clicks (Button2) across it with distinct synthetic event times. The
task-button hit kills via kill_client_ex, and the WM_DELETE
ClientMessage must carry that same non-zero time in data.l[1].

Prints KILL_STAMP=<n> STAMP_OK=0/1.
"""
import os
import select
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


def find_bar(d, root):
    try:
        kids = root.query_tree().children
    except Exception:
        return None, 0, 0
    for w in kids:
        try:
            a = w.get_attributes()
            g = w.get_geometry()
        except Exception:
            continue
        if a.override_redirect and g.height < 60 and g.width >= 800:
            return w, g.width, g.height
    return None, 0, 0


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    protos_atom = d.intern_atom("WM_PROTOCOLS")
    del_atom = d.intern_atom("WM_DELETE_WINDOW")

    w = root.create_window(
        10, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
    )
    w.set_wm_name("kill-stamp-repro")
    w.set_wm_protocols([del_atom])
    w.map()
    d.flush()
    if wait_for(root, net_client_list, 1) != 1:
        print("SETUP-FAIL: never managed", file=sys.stderr)
        return 2
    time.sleep(0.8)

    bar, bw, bh = find_bar(d, root)
    if bar is None:
        print("SETUP-FAIL: bar not found", file=sys.stderr)
        return 2
    print("BAR=%d %dx%d" % (bar.id, bw, bh))
    sys.stdout.flush()

    fd = d.fileno()
    stamp = 0
    base = 2000000
    y = max(1, bh // 2)
    for i, x in enumerate(range(0, bw, 8)):
        t = base + i
        ev = event.ButtonPress(
            detail=2, time=t, root=root, window=bar, child=X.NONE,
            root_x=x, root_y=y, event_x=x, event_y=y,
            state=0, same_screen=1,
        )
        bar.send_event(ev, event_mask=X.ButtonPressMask)
        d.flush()
        deadline = time.time() + 0.06
        while time.time() < deadline:
            while d.pending_events():
                e = d.next_event()
                if e.type == X.ClientMessage:
                    try:
                        mt = e.client_type
                        mt = mt.id if hasattr(mt, "id") else int(mt)
                    except Exception:
                        continue
                    if mt == protos_atom:
                        try:
                            data = e.data[1]
                        except Exception:
                            continue
                        if data and int(data[0]) == int(del_atom):
                            stamp = int(data[1])
                            break
                # ButtonPress sent to bar may echo? ignore others.
            if stamp:
                break
            r, _, _ = select.select([fd], [], [], 0.02)
            if not r:
                continue
        if stamp:
            break
    print("KILL_STAMP=%d" % stamp)
    print("STAMP_OK=%d" % (1 if stamp != 0 else 0))
    sys.stdout.flush()

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
