#!/usr/bin/env python3
"""Check protocol delivery independently of actual focus and keyboard delivery.

Endorsers deliberately never respond by setting input focus themselves.
"""
import os
import sys
import time

from Xlib import X, XK, Xutil, display
from Xlib.ext import xtest


def main():
    d = display.Display(os.environ.get("DISPLAY"))
    root = d.screen().root
    clients = d.intern_atom("_NET_CLIENT_LIST")
    active = d.intern_atom("_NET_ACTIVE_WINDOW")
    protocols = d.intern_atom("WM_PROTOCOLS")
    take = d.intern_atom("WM_TAKE_FOCUS")
    windows = []

    def wid(value):
        return value.id if hasattr(value, "id") else int(value)

    def focus():
        return wid(d.get_input_focus().focus)

    def drain():
        d.sync()
        events = []
        while d.pending_events():
            events.append(d.next_event())
        return events

    def wait(predicate, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return True
            time.sleep(0.02)
        return False

    def message(ev, w):
        return (ev.type == X.ClientMessage and wid(ev.window) == w.id
                and ev.client_type == protocols and ev.data[0] == 32
                and int(ev.data[1][0]) == take)

    def result(name, ok):
        print("%s=%d" % (name, bool(ok)), flush=True)
        if not ok:
            print("%s: actual focus=%s" % (name, focus()), file=sys.stderr)

    def create(name, wants=True, endorses=True):
        w = root.create_window(
            10, 10, 200, 150, 0, d.screen().root_depth,
            X.InputOutput, X.CopyFromParent, background_pixel=0xFFFFFF,
            event_mask=X.StructureNotifyMask | X.KeyPressMask)
        windows.append(w)
        w.set_wm_name(name)
        w.set_wm_hints(input=wants, flags=Xutil.InputHint)
        if endorses:
            w.set_wm_protocols([take])
        w.map()
        d.flush()

        def managed():
            prop = root.get_full_property(clients, X.AnyPropertyType)
            return prop is not None and w.id in prop.value

        if not wait(managed):
            raise RuntimeError("window never managed: " + name)
        return w

    try:
        # Keep the pointer outside clients during map/layout changes.
        xtest.fake_input(d, X.MotionNotify, x=1279, y=799)
        d.sync()
        baseline = create("takefocus-baseline", endorses=False)
        if not wait(lambda: focus() == baseline.id):
            raise RuntimeError("baseline did not receive focus")
        w1 = create("takefocus-w1")
        w2 = create("takefocus-w2")
        # Require geometry to remain stable before calculating hover targets.
        previous = None
        stable_since = time.monotonic()

        def stable():
            nonlocal previous, stable_since
            geometries = [w.get_geometry() for w in (w1, w2)]
            geometry = [(g.x, g.y, g.width, g.height) for g in geometries]
            if geometry != previous:
                previous = geometry
                stable_since = time.monotonic()
            return time.monotonic() - stable_since >= 0.4

        if not wait(stable):
            raise RuntimeError("layout did not settle")

        for label, target in (("W1", w1), ("W2", w2), ("W1_RETURN", w1)):
            drain()  # No map-time messages may satisfy this phase.
            geo = target.get_geometry()
            pos = root.translate_coords(target, geo.width // 2, geo.height // 2)
            xtest.fake_input(d, X.MotionNotify, x=pos.x, y=pos.y)
            d.flush()
            events = []

            def focused():
                events.extend(drain())
                return focus() == target.id and any(message(e, target) for e in events)

            wait(focused)
            result(label + "_MESSAGE_OK", any(message(e, target) for e in events))
            result(label + "_FOCUS_OK", focus() == target.id)
            drain()
            key = d.keysym_to_keycode(XK.string_to_keysym("a"))
            xtest.fake_input(d, X.KeyPress, key)
            xtest.fake_input(d, X.KeyRelease, key)
            d.flush()
            keys = []

            def delivered():
                keys.extend(e for e in drain() if e.type == X.KeyPress)
                return bool(keys)

            wait(delivered)
            result(label + "_KEY_OK", bool(keys) and
                   all(wid(e.window) == target.id and e.detail == key for e in keys))

        normal = create("takefocus-normal", endorses=False)
        result("NORMAL_FOCUS_OK", wait(lambda: focus() == normal.id))
        drain()
        noinput = create("takefocus-noinput", wants=False)
        events = []

        def notified():
            events.extend(drain())
            return any(message(e, noinput) for e in events)

        result("NOINPUT_MESSAGE_OK", wait(notified))
        # Observe a bounded interval too: delayed focus must not be forced.
        stolen = False
        until = time.monotonic() + 0.4
        while time.monotonic() < until:
            stolen |= focus() != normal.id
            time.sleep(0.02)
        result("NOINPUT_FOCUS_OK", not stolen)
        prop = root.get_full_property(active, X.AnyPropertyType)
        result("NOINPUT_ACTIVE_OK", prop is not None and int(prop.value[0]) == noinput.id)
    finally:
        for w in windows:
            w.destroy()
        d.sync()
        d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
