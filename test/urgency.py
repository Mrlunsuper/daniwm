#!/usr/bin/env python3
"""T-M2 helper: urgency mirrors hint + DA, focused never urgent.

Maps A/B, focuses A. Direct-writes DA on unfocused B (add/remove),
sets/clears XUrgencyHint on B, then focuses B to prove focus clears.

Prints DA_ADD_OK DA_REMOVE_OK HINT_SET_OK HINT_CLEAR_OK FOCUS_CLEAR_OK.
"""
import os
import sys
import time

from Xlib import X, display, Xutil
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
    for _ in range(tries):
        if client_list_len(root, atom) == want:
            return want
        time.sleep(0.1)
    return client_list_len(root, atom)


def wait_state(win, prop, atom, want, tries=50):
    for _ in range(tries):
        if has_state(win, prop, atom) == want:
            return 1
        time.sleep(0.1)
    return 0


def request_active(d, root, net_active, win):
    ev = event.ClientMessage(
        window=win, client_type=net_active, format=32,
        data=(32, [1, 0, 0, 0, 0]),
    )
    root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    d.flush()


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    net_active = d.intern_atom("_NET_ACTIVE_WINDOW")
    net_wm_state = d.intern_atom("_NET_WM_STATE")
    da_atom = d.intern_atom("_NET_WM_STATE_DEMANDS_ATTENTION")
    atom_type = d.intern_atom("ATOM")

    a = root.create_window(10, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent, background_pixel=0xFFFFFF,
        event_mask=X.StructureNotifyMask)
    a.set_wm_name("urg-A")
    a.map()
    d.flush()
    time.sleep(0.3)
    b = root.create_window(320, 10, 200, 150, 0, d.screen().root_depth,
        X.InputOutput, X.CopyFromParent, background_pixel=0xFFFFFF,
        event_mask=X.StructureNotifyMask)
    b.set_wm_name("urg-B")
    b.map()
    d.flush()
    if wait_for(root, net_client_list, 2) != 2:
        print("SETUP-FAIL: never managed", file=sys.stderr)
        return 2
    request_active(d, root, net_active, a)
    time.sleep(0.8)

    # Direct DA add/remove on unfocused B.
    b.change_property(net_wm_state, atom_type, 32, [int(da_atom)])
    d.flush()
    da_add = wait_state(b, net_wm_state, da_atom, 1)
    print("DA_ADD_OK=%d" % da_add)
    b.delete_property(net_wm_state)
    d.flush()
    da_rm = wait_state(b, net_wm_state, da_atom, 0)
    print("DA_REMOVE_OK=%d" % da_rm)

    # XUrgencyHint set/clear on unfocused B.
    b.set_wm_hints(flags=Xutil.InputHint | Xutil.UrgencyHint, input=1)
    d.flush()
    hint_set = wait_state(b, net_wm_state, da_atom, 1)
    print("HINT_SET_OK=%d" % hint_set)
    b.set_wm_hints(flags=Xutil.InputHint, input=1)
    d.flush()
    hint_clear = wait_state(b, net_wm_state, da_atom, 0)
    print("HINT_CLEAR_OK=%d" % hint_clear)

    # Focus clears urgency.
    b.set_wm_hints(flags=Xutil.InputHint | Xutil.UrgencyHint, input=1)
    d.flush()
    wait_state(b, net_wm_state, da_atom, 1)
    request_active(d, root, net_active, b)
    time.sleep(0.8)
    focus_clear = 1 if has_state(b, net_wm_state, da_atom) == 0 else 0
    print("FOCUS_CLEAR_OK=%d" % focus_clear)
    sys.stdout.flush()

    for w in (a, b):
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
