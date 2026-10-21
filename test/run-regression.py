#!/usr/bin/env python3
"""dani-run regression; only temporary data and harmless commands."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
from Xlib import X, display

binary = os.environ.get('RUN_BIN', str(Path(__file__).resolve().parent.parent / 'dani-run'))
with tempfile.TemporaryDirectory() as tmp:
    home = Path(tmp)
    user = home / 'data/applications'
    system = home / 'system/applications'
    user.mkdir(parents=True)
    system.mkdir(parents=True)
    config = home / 'config/daniwm'
    config.mkdir(parents=True)
    env = dict(os.environ, HOME=tmp, XDG_CONFIG_HOME=str(home / 'config'),
               XDG_DATA_HOME=str(home / 'data'), XDG_DATA_DIRS=str(home / 'system'),
               XDG_CACHE_HOME=str(home / 'cache'), SHELL='/bin/sh')

    def desktop(path, name, extra='', command='/bin/true'):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f'[Desktop Entry]\nType=Application\nName={name}\n'
                        + (f'Exec={command}\n' if command else '') + extra)

    desktop(system / 'hidden.desktop', 'Hidden system')
    desktop(user / 'hidden.desktop', 'Hidden user', 'Hidden=true\n')
    desktop(system / 'override.desktop', 'System override')
    desktop(user / 'override.desktop', 'User override')
    desktop(user / 'nested/app.desktop', 'Nested')
    desktop(user / 'same1.desktop', 'Same name', command='/bin/true')
    desktop(user / 'same2.desktop', 'Same name', command='/bin/false')
    desktop(user / 'actions.desktop', 'Actions',
            '[Desktop Action other]\nExec=/bin/false\nNoDisplay=true\nTerminal=true\n')
    desktop(user / 'noexec.desktop', 'No main exec',
            '[Desktop Action other]\nExec=/bin/false\n', command='')
    desktop(user / 'fields.desktop', 'Field App', 'Icon=field-icon\n',
            '/bin/echo %c %k %i %% "$HOME" "*" "~"')
    (user / 'wrongtype.desktop').write_text('[Desktop Entry]\nType=Link\nName=Link\nExec=/bin/true\n')
    lines = subprocess.check_output([binary, '--list'], env=env, text=True).splitlines()
    pairs = [s.split('\t', 1) for s in lines]
    names = [p[0] for p in pairs]
    assert 'Hidden system' not in names and 'Hidden user' not in names
    assert 'User override' in names and 'System override' not in names
    assert 'Nested' in names and names.count('Same name') == 2
    assert 'Actions' in names and 'No main exec' not in names and 'Link' not in names
    cmd = next(cmd for name, cmd in pairs if name == 'Field App')
    assert shlex.split(cmd) == ['/bin/echo', 'Field App', str(user / 'fields.desktop'),
                                 '--icon', 'field-icon', '%', '$HOME', '*', '~']
    print('PASS: desktop sections, IDs, Hidden mask, XDG roots, recursion and argv')
    (config / 'run.config').write_text('app = ;Hash;/bin/echo "a#b"\n')
    assert '"a#b"' in subprocess.check_output([binary, '--fav', '--list'], env=env, text=True)
    print('PASS: hash preserved in config command')

    number = next(n for n in range(50, 200) if not Path(f'/tmp/.X{n}-lock').exists()
                  and not Path(f'/tmp/.X11-unix/X{n}').exists())
    env['DISPLAY'] = f':{number}'
    xvfb = subprocess.Popen(['Xvfb', env['DISPLAY'], '-screen', '0', '1280x800x24'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = None
    d = None
    try:
        time.sleep(.7)
        d = display.Display(env['DISPLAY'])
        root = d.screen().root

        def start(flag):
            p = subprocess.Popen([binary, flag], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
            time.sleep(.4)
            assert p.poll() is None, p.communicate()
            return p

        def key(*args):
            subprocess.run(['xdotool', *args], env=env, check=True)

        # 8 compact rows are accepted, but paginate whole rows to screen height.
        (config / 'run.config').write_text(
            'cols = 5\nlines = 8\ncell_w = 36\ncell_h = 112\n'
            'cell_gap_x = 8\ncell_gap_y = 6\n')
        proc = start('--fav')
        w = next(w for w in root.query_tree().children if (w.get_wm_name() or '').startswith('dani-run'))
        g = w.get_geometry()
        assert g.width == 244 and g.height == 654, (g.width, g.height)
        key('key', 'Escape')
        proc.communicate(timeout=2)
        (config / 'run.config').write_text(
            'cols = 5\nlines = 8\ncell_w = 180\n'
            'power_cols = 5\npower_lines = 1\npower_cell_w = 112\npower_cell_h = 96\n')
        proc = start('--power')
        w = next(w for w in root.query_tree().children if (w.get_wm_name() or '') == 'dani-run power')
        g = w.get_geometry()
        assert g.width == 592 and g.height == 166, (g.width, g.height)
        key('key', 'Escape')
        proc.communicate(timeout=2)
        print('PASS: power grid overrides do not inherit oversized favorite layout')
        for gap in (0, 20):
            (config / 'run.config').write_text(
                f'width = 600\ndrun_lines = 5\nrow_gap = {gap}\n')
            proc = subprocess.Popen(
                [binary], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(.4)
            w = next(w for w in root.query_tree().children if (w.get_wm_name() or '') == 'dani-run')
            g = w.get_geometry()
            assert g.width == 600 and g.height == 70 + 5 * (34 + gap)
            key('key', 'Escape')
            proc.communicate(timeout=2)
        print('PASS: compact grid geometry, whole-row screen fit and real row gaps')

        for flag in ('--fav', '--power'):
            (config / 'run.config').write_text(
                f'app = ;Safe;/usr/bin/touch {home}/clicked\n'
                f'power = ;Safe;/usr/bin/touch {home}/clicked\n')
            proc = start(flag)
            w = next(w for w in root.query_tree().children if (w.get_wm_name() or '').startswith('dani-run'))
            g = w.get_geometry()
            key('mousemove', str(g.x + 40), str(g.y + 43), 'click', '1')
            time.sleep(.2)
            assert proc.poll() is None and not (home / 'clicked').exists()
            key('mousemove', str(g.x + 40), str(g.y + 60), 'click', '1')
            proc.communicate(timeout=3)
            for _ in range(30):
                if (home / 'clicked').exists(): break
                time.sleep(.05)
            assert (home / 'clicked').exists()
            (home / 'clicked').unlink()
        print('PASS: fav/power input clicks do nothing; valid cells still activate')

        tools = home / 'bin'
        tools.mkdir()
        for name, script in [('xclip', '#!/bin/sh\nsleep 4\n'),
                             ('xsel', f'#!/bin/sh\ncat > {home}/clipboard\n')]:
            p = tools / name
            p.write_text(script)
            p.chmod(0o755)
        env['PATH'] = str(tools) + ':' + os.environ['PATH']
        for failing_clip in ('#!/bin/sh\nsleep 4\n', '#!/bin/sh\nexit 1\n'):
            (tools / 'xclip').write_text(failing_clip)
            proc = start('--calc')
            key('type', '2^-2')
            start_time = time.monotonic()
            key('key', 'Return')
            out, err = proc.communicate(timeout=2)
            assert proc.returncode == 0 and out.strip() == '0.25', (out, err)
            assert time.monotonic() - start_time < 1
            for _ in range(50):
                if (home / 'clipboard').exists(): break
                time.sleep(.05)
            assert (home / 'clipboard').read_text() == '0.25'
            (home / 'clipboard').unlink()
        print('PASS: clipboard timeout/failure fallback and prompt calculator exit')

        # A competing grab must cause a bounded failure, not a dead launcher.
        grab = root.grab_keyboard(False, X.GrabModeAsync, X.GrabModeAsync, X.CurrentTime)
        assert grab == X.GrabSuccess
        d.sync()
        proc = subprocess.Popen([binary, '--fav'], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        _, err = proc.communicate(timeout=4)
        assert proc.returncode != 0 and 'cannot grab keyboard' in err
        d.ungrab_keyboard(X.CurrentTime)
        d.sync()
        print('PASS: keyboard grab failure exits cleanly')
        print('RESULT: PASS')
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=3)
        if d is not None:
            d.close()
        xvfb.terminate()
        xvfb.wait(timeout=3)
