#!/usr/bin/env python3
"""T-L4 helper: reload must not over-map monocle-hidden windows.

Maps A/B/C, focuses A (monocle will show A), prints IDS, then sleeps
so the driver script can switch to monocle and reload the config.
After the sleep it asserts A viewable, B/C hidden, list still 3.

The helper stays alive throughout: closing the Display would destroy
its windows (X frees a client's resources on disconnect).
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


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    net_client_list = d.intern_atom("_NET_CLIENT_LIST")
    net_active = d.intern_atom("_NET_ACTIVE_WINDOW")

    wins = []
    for i in range(3):
        w = root.create_window(
            10 + i * 20, 10, 200, 150, 0, d.screen().root_depth,
            X.InputOutput, X.CopyFromParent,
            background_pixel=0xFFFFFF, event_mask=X.StructureNotifyMask,
        )
        w.set_wm_name("reload-repro-%d" % i)
        w.map()
        d.flush()
        wins.append(w)
    if wait_for(root, net_client_list, 3) != 3:
        print("SETUP-FAIL: 3 windows never managed", file=sys.stderr)
        return 2
    ev = event.ClientMessage(
        window=wins[0], client_type=net_active, format=32,
        data=(32, [1, 0, 0, 0, 0]),
    )
    root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    d.flush()
    time.sleep(0.8)
    print("IDS=%d,%d,%d" % (wins[0].id, wins[1].id, wins[2].id))
    sys.stdout.flush()

    time.sleep(6)  # driver: super+m, reload, settle

    states = []
    for w in wins:
        try:
            states.append(w.get_attributes().map_state)
        except Exception:
            states.append(-1)
    print("STATES=%s" % ",".join(str(s) for s in states))
    n = client_list_len(root, net_client_list)
    print("LIST=%d" % n)
    ok = 1 if (states[0] == X.IsViewable and states[1] == X.IsUnmapped and
               states[2] == X.IsUnmapped and n == 3) else 0
    print("RELOAD_OK=%d" % ok)
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
