#!/usr/bin/env python3
"""T-L2 helper: floating ConfigureRequest storm stays clamped.

Maps a DIALOG-type (floating) window, then storms ConfigureRequests
with 0 / huge sizes. The WM must clamp its cache: final geometry stays
w,h >= 1 and bounded (<= 2x screen), the window stays managed, the WM
stays alive. No X error kills the WM (H3 traps make faults loud).

Prints FINAL_GEOM=<x,y,wxh> CLAMP_OK=0/1.
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


def wait_for(root, atom, want, tries=100):
    for _ in range(tries):
        if client_list_len(root, atom) == want:
            return want
        time.sleep(0.1)
    return client_list_len(root, atom)


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    scr = d.screen()
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    wtype = d.intern_atom("_NET_WM_WINDOW_TYPE")
    dialog = d.intern_atom("_NET_WM_WINDOW_TYPE_DIALOG")

    w = root.create_window(
        10, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
    )
    w.set_wm_name("clamp-float-repro")
    w.change_property(wtype, d.intern_atom("ATOM"), 32, [int(dialog)])
    w.map()
    d.flush()
    if wait_for(root, net_client_list, 1) != 1:
        print("SETUP-FAIL: never managed", file=sys.stderr)
        return 2
    time.sleep(0.8)

    maxw, maxh = scr.width_in_pixels * 2, scr.height_in_pixels * 2
    # Storm via a separate connection: width=0 makes the server report
    # BadValue on the sender, which would poison that connection for
    # later round-trips (python-xlib mis-parses async errors). The WM
    # still receives the ConfigureRequest and must clamp its cache.
    # All observations stay on the clean primary connection.
    d2 = display.Display(os.environ.get("DISPLAY"))
    w2 = d2.create_resource_object("window", w.id)
    storms = [
        dict(width=0, height=0),
        dict(width=1, height=1),
        dict(width=100000, height=100000),
        dict(x=-5000, y=-5000, width=50, height=50),
        dict(width=0, height=300),
        dict(width=300, height=0),
    ]
    for s in storms:
        try:
            w2.configure(**s)
            d2.flush()
        except Exception:
            pass
        time.sleep(0.15)
    time.sleep(0.8)
    try:
        d2.close()
    except Exception:
        pass

    try:
        g = w.get_geometry()
        print("FINAL_GEOM=%d,%d %dx%d" % (g.x, g.y, g.width, g.height))
        ok = 1 if (g.width >= 1 and g.height >= 1 and
                   g.width <= maxw and g.height <= maxh) else 0
    except Exception as e:
        print("FAIL: geometry unreadable: %s" % e, file=sys.stderr)
        ok = 0
    print("CLAMP_OK=%d" % ok)
    alive = 1 if client_list_len(root, net_client_list) == 1 else 0
    print("MANAGED_OK=%d" % alive)
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
