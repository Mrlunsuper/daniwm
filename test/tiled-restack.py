#!/usr/bin/env python3
"""audit-0930 #10: a tiled client's own stacking request must be ignored
(tiling owns the stack) and can never put it above a floating window."""
import time
from Xlib import X, display
d = display.Display(); r = d.screen().root
def mk(name, w_, h_, trans=None):
    w = r.create_window(10, 10, w_, h_, 0, d.screen().root_depth, X.InputOutput,
                        X.CopyFromParent, background_pixel=0)
    w.set_wm_name(name)
    if trans is not None: w.set_wm_transient_for(trans)
    w.map(); d.sync(); time.sleep(0.8); return w
def idx(ws):
    kids = r.query_tree().children
    return [kids.index(w) for w in ws]
a = mk("TR-A", 300, 200)
b = mk("TR-B", 300, 200)
f = mk("TR-F", 100, 100, trans=b)   # transient -> floating
d.sync(); time.sleep(0.5)
ai, bi, fi = idx([a, b, f])
print("before: A=%d B=%d F=%d" % (ai, bi, fi))
b.configure(stack_mode=X.Below)     # tiled asks to go under its twin
d.sync(); time.sleep(0.8)
ai2, bi2, fi2 = idx([a, b, f])
print("after:  A=%d B=%d F=%d" % (ai2, bi2, fi2))
ok = (ai2 < bi2) == (ai < bi) and fi2 > ai2 and fi2 > bi2
print("PASS: tiled self-restack ignored" if ok
      else "FAIL: tiled restacked itself / rose above float")
