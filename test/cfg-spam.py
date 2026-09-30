#!/usr/bin/env python3
"""audit-0930 #7 measurement: N tiled clients spam ConfigureRequests; time
until the WM drains the backlog (probe: _NET_CURRENT_DESKTOP round trip)."""
import sys, time
from Xlib import X, display, protocol
N = int(sys.argv[1]) if len(sys.argv) > 1 else 500
d = display.Display(); r = d.screen().root
A = lambda n: d.intern_atom(n)
ws = []
for i in range(3):
    w = r.create_window(10, 10, 300, 200, 0, d.screen().root_depth, X.InputOutput,
                        X.CopyFromParent, background_pixel=0)
    w.set_wm_name("SPAM%d" % i); w.map(); ws.append(w)
d.sync(); time.sleep(1.5)
def cur():
    p = r.get_full_property(A("_NET_CURRENT_DESKTOP"), X.AnyPropertyType)
    return p.value[0]
t0 = time.time()
for i in range(N):
    ws[i % 3].configure(width=200 + i % 50, height=150 + i % 40)
target = 1 - cur()
ev = protocol.event.ClientMessage(window=r, client_type=A("_NET_CURRENT_DESKTOP"),
                                  data=(32, [target, 0, 0, 0, 0]))
r.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
d.sync()
while cur() != target: time.sleep(0.002)
print("drain_ms=%d" % ((time.time() - t0) * 1000))
