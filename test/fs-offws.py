#!/usr/bin/env python3
"""audit-0930 #5: fullscreen request from a window on another workspace
must not focus it (it is unmapped there)."""
import time
from Xlib import X, display, protocol
d = display.Display(); r = d.screen().root
A = lambda n: d.intern_atom(n)
def cm(win, typ, data):
    ev = protocol.event.ClientMessage(window=win, client_type=A(typ), data=(32, data))
    r.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    d.sync(); time.sleep(1)
w = r.create_window(10, 10, 300, 200, 0, d.screen().root_depth, X.InputOutput,
                    X.CopyFromParent, background_pixel=0)
w.set_wm_name("FSTEST"); w.map(); d.sync(); time.sleep(1)
cm(r, "_NET_CURRENT_DESKTOP", [1, 0, 0, 0, 0])
cm(w, "_NET_WM_STATE", [1, A("_NET_WM_STATE_FULLSCREEN"), 0, 1, 0])
p = r.get_full_property(A("_NET_ACTIVE_WINDOW"), X.AnyPropertyType)
act = p.value[0] if p and len(p.value) else 0
print("PASS: off-ws fullscreen keeps focus" if act != w.id
      else "FAIL: off-ws window became active")
