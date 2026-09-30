#!/usr/bin/env python3
"""audit-0930 #4: a window the client withdrew must stay unmapped across
restart (the shell script restarts the WM ~2.5s after launch)."""
import time
from Xlib import X, display
d = display.Display(); r = d.screen().root
w = r.create_window(10, 10, 300, 200, 0, d.screen().root_depth, X.InputOutput,
                    X.CopyFromParent, background_pixel=0)
w.set_wm_name("WDTEST"); w.map(); d.sync(); time.sleep(1)
w.unmap(); d.sync(); time.sleep(5)
st = w.get_attributes().map_state
print("PASS: withdrawn stays unmapped" if st == X.IsUnmapped
      else "FAIL: withdrawn remapped (state=%d)" % st)
