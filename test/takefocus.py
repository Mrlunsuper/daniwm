#!/usr/bin/env python3
"""T-H2.3 helper: WM_TAKE_FOCUS ClientMessage delivery.

Maps a window endorsing WM_TAKE_FOCUS in WM_PROTOCOLS, waits for the
WM to manage/focus it, and listens for the WM_TAKE_FOCUS ClientMessage.
Then maps a normal window and checks it still gets XSetInputFocus.

Prints TAKE_FOCUS_GOT=0/1 NORMAL_FOCUS_OK=0/1.
"""
import os
import select
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


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    net_active = d.intern_atom("_NET_ACTIVE_WINDOW")
    protos_atom = d.intern_atom("WM_PROTOCOLS")
    take_atom = d.intern_atom("WM_TAKE_FOCUS")

    t = root.create_window(
        10, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0x00FF00, event_mask=X.StructureNotifyMask,
    )
    t.set_wm_name("takefocus-endorser")
    t.set_wm_protocols([take_atom])
    t.map()
    d.flush()

    if wait_for(root, net_client_list, 1, tries=100) != 1:
        print("SETUP-FAIL: takefocus window never managed", file=sys.stderr)
        return 2
    print("T=%d" % t.id)

    # Manage() focuses immediately, so the message may already be queued.
    got = 0
    fd = d.fileno()
    deadline = time.time() + 5.0
    while time.time() < deadline and not got:
        while d.pending_events():
            ev = d.next_event()
            if ev.type == X.ClientMessage:
                try:
                    mt = ev.client_type
                    mt = mt.id if hasattr(mt, "id") else int(mt)
                except Exception:
                    continue
                if mt == protos_atom:
                    try:
                        data = ev.data[1]
                    except Exception:
                        continue
                    if data and int(data[0]) == take_atom:
                        got = 1
                        break
        if got:
            break
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            continue
    print("TAKE_FOCUS_GOT=%d" % got)
    print("TAKE_ACTIVE=%d" % active_window(root, net_active))
    sys.stdout.flush()

    # Normal client keeps the XSetInputFocus path.
    n = root.create_window(
        320, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
    )
    n.set_wm_name("takefocus-normal")
    n.map()
    d.flush()
    if wait_for(root, net_client_list, 2, tries=100) != 2:
        print("SETUP-FAIL: normal window never managed", file=sys.stderr)
        return 2
    time.sleep(0.8)
    rf = get_focus(d)
    ra = active_window(root, net_active)
    print("NORMAL_FOCUS=%d" % rf)
    print("NORMAL_ACTIVE=%d" % ra)
    print("NORMAL_FOCUS_OK=%d" % (1 if (rf == n.id and ra == n.id) else 0))
    sys.stdout.flush()

    for w in (t, n):
        try:
            w.destroy()
        except Exception:
            pass
    d.flush()
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
