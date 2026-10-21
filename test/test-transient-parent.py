#!/usr/bin/env python3
"""Isolated regression: transient desktop inheritance and override precedence."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

from Xlib import X, display, protocol

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as home:
    number = next(n for n in range(50, 200)
                  if not Path(f'/tmp/.X{n}-lock').exists()
                  and not Path(f'/tmp/.X11-unix/X{n}').exists())
    env = dict(os.environ, DISPLAY=f':{number}', HOME=home,
               XDG_CONFIG_HOME=f'{home}/.config')
    config = Path(home) / '.config/daniwm'
    config.mkdir(parents=True)
    (config / 'config').write_text('rule = Override:*:float:3\n')
    xvfb = subprocess.Popen(['Xvfb', env['DISPLAY'], '-screen', '0', '1280x800x24'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wm = None
    d = None
    try:
        time.sleep(1)
        wm = subprocess.Popen([os.environ.get('WM_BIN', str(repo / 'daniwm'))], env=env)
        time.sleep(1)
        assert wm.poll() is None, 'WM failed to start'
        d = display.Display(env['DISPLAY'])
        root = d.screen().root
        atom = d.intern_atom

        def settle():
            d.sync()
            time.sleep(.3)

        def prop(w, name):
            p = w.get_full_property(atom(name), X.AnyPropertyType)
            return int(p.value[0]) if p is not None else None

        def create(name, parent=None, desktop=None, cls='Plain'):
            w = root.create_window(20, 20, 300, 180, 0, d.screen().root_depth,
                                   X.InputOutput, X.CopyFromParent, background_pixel=0)
            w.set_wm_name(name)
            w.set_wm_class(cls, cls)
            if parent is not None:
                w.set_wm_transient_for(parent)
            if desktop is not None:
                w.change_property(atom('_NET_WM_DESKTOP'), atom('CARDINAL'), 32, [desktop])
            w.map()
            settle()
            return w

        parent = create('parent')
        root.send_event(protocol.event.ClientMessage(
            window=root, client_type=atom('_NET_CURRENT_DESKTOP'),
            data=(32, [1, 0, 0, 0, 0])),
            event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
        settle()
        active = create('current workspace client')
        dialog = create('off-workspace dialog', parent)
        assert prop(dialog, '_NET_WM_DESKTOP') == 0
        assert dialog.get_attributes().map_state == X.IsUnmapped
        assert prop(root, '_NET_CURRENT_DESKTOP') == 1
        assert prop(root, '_NET_ACTIVE_WINDOW') == active.id
        print('PASS: off-workspace dialog follows parent without stealing focus')

        nested = create('nested dialog', dialog)
        assert prop(nested, '_NET_WM_DESKTOP') == 0
        print('PASS: nested transient inherits parent workspace')

        ruled = create('rule override', parent, cls='Override')
        assert prop(ruled, '_NET_WM_DESKTOP') == 2
        print('PASS: configured workspace overrides inherited workspace')

        explicit = create('desktop override', parent, desktop=1, cls='Override')
        assert prop(explicit, '_NET_WM_DESKTOP') == 1
        assert explicit.get_attributes().map_state == X.IsViewable
        print('PASS: explicit desktop overrides rule and inheritance')

        unknown = create('root transient', root)
        assert prop(unknown, '_NET_WM_DESKTOP') == 1
        print('PASS: unmanaged transient parent falls back to current workspace')
        print('RESULT: PASS')
    finally:
        if d is not None:
            d.close()
        if wm is not None and wm.poll() is None:
            wm.terminate()
            wm.wait(timeout=5)
        xvfb.terminate()
        xvfb.wait(timeout=5)
