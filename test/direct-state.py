#!/usr/bin/env python3
"""T-M1 helper: direct _NET_WM_STATE property writes converge.

Maps one window, records tiled geometry, then writes _NET_WM_STATE
with FULLSCREEN directly (no ClientMessage). Expects the WM to
reconcile: fullscreen state present and geometry fullscreen. Then
deletes the property and expects a return to tiled. Finally toggles
via ClientMessage to prove the canonical path still works.

Prints TILED_W/H FS_W/H BACK_W/H, FS_PROP=0/1 BACK_PROP=0/1,
CM_FS=0/1, and DIRECT_OK/CM_OK.
"""
import os
import sys
import time

from Xlib import X, display
from Xlib.protocol import event


def has_state(win, prop, atom):
    p = win.get_full_property(prop, X.AnyPropertyType)
    if p is None or not p.value:
        return 0
    return 1 if int(atom) in [int(v) for v in p.value] else 0


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


def geom(win):
    g = win.get_geometry()
    return g.x, g.y, g.width, g.height


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    scr = d.screen()
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    net_wm_state = d.intern_atom("_NET_WM_STATE")
    fs_atom = d.intern_atom("_NET_WM_STATE_FULLSCREEN")

    win = root.create_window(
        10, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
    )
    win.set_wm_name("direct-state-repro")
    win.map()
    d.flush()
    if wait_for(root, net_client_list, 1) != 1:
        print("SETUP-FAIL: never managed", file=sys.stderr)
        return 2
    time.sleep(0.8)
    tx, ty, tw, th = geom(win)
    print("TILED=%d,%d %dx%d" % (tx, ty, tw, th))

    # Direct write: FULLSCREEN without ClientMessage.
    win.change_property(net_wm_state, Xatom_atom(d), 32, [fs_atom])
    d.flush()
    fs_prop = 0
    fx = fy = fw = fh = 0
    for _ in range(100):
        time.sleep(0.1)
        fs_prop = has_state(win, net_wm_state, fs_atom)
        fx, fy, fw, fh = geom(win)
        if fs_prop and fw == scr.width_in_pixels and fh == scr.height_in_pixels:
            break
    print("FS_PROP=%d" % fs_prop)
    print("FS_GEOM=%d,%d %dx%d" % (fx, fy, fw, fh))
    direct_fs = 1 if (fs_prop and fw == scr.width_in_pixels) else 0

    # Direct removal: back to tiled.
    win.delete_property(net_wm_state)
    d.flush()
    back_prop = 1
    bx = by = bw = bh = fx, fy, fw, fh
    for _ in range(100):
        time.sleep(0.1)
        back_prop = has_state(win, net_wm_state, fs_atom)
        bx, by, bw, bh = geom(win)
        if not back_prop and bw < scr.width_in_pixels:
            break
    print("BACK_PROP=%d" % back_prop)
    print("BACK_GEOM=%d,%d %dx%d" % (bx, by, bw, bh))
    direct_back = 1 if (not back_prop and bw < scr.width_in_pixels) else 0
    print("DIRECT_OK=%d" % (1 if (direct_fs and direct_back) else 0))

    # Canonical ClientMessage path still works (_ADD then _REMOVE).
    for act, want in ((1, 1), (0, 0)):
        ev = event.ClientMessage(
            window=win, client_type=net_wm_state, format=32,
            data=(32, [act, int(fs_atom), 0, 0, 0]),
        )
        root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
        d.flush()
        ok = 0
        for _ in range(100):
            time.sleep(0.1)
            if has_state(win, net_wm_state, fs_atom) == want:
                ok = 1
                break
        if not ok:
            print("CM_FS=0")
            print("CM_OK=0")
            win.destroy()
            d.flush()
            d.close()
            return 0
    print("CM_FS=1")
    print("CM_OK=1")
    sys.stdout.flush()

    win.destroy()
    d.flush()
    wait_for(root, net_client_list, 0)
    d.close()
    return 0


def Xatom_atom(d):
    return d.intern_atom("ATOM")


if __name__ == "__main__":
    sys.exit(main())
