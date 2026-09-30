#!/usr/bin/env python3
"""audit-0930 #1: pager _NET_ACTIVE_WINDOW long after the last key press
must still move real X input focus (stale last_evtime was rejected)."""
import subprocess, time
from Xlib import X, display, protocol
d = display.Display(); r = d.screen().root
A = lambda n: d.intern_atom(n)
def mk(n):
    w = r.create_window(10, 10, 300, 200, 0, d.screen().root_depth, X.InputOutput,
                        X.CopyFromParent, background_pixel=0)
    w.set_wm_name(n); w.map(); d.sync(); time.sleep(0.8); return w
a = mk("A"); b = mk("B")
subprocess.run(["xdotool", "key", "super+j"]); time.sleep(2)   # WM records key time
a.set_input_focus(X.RevertToPointerRoot, X.CurrentTime); d.sync(); time.sleep(0.5)
ev = protocol.event.ClientMessage(window=b, client_type=A("_NET_ACTIVE_WINDOW"),
                                  data=(32, [2, 0, 0, 0, 0]))
r.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
d.sync(); time.sleep(0.8)
ok = d.get_input_focus().focus.id == b.id
print("PASS: pager activate moves real focus" if ok else "FAIL: pager activate moves real focus")
