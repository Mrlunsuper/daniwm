#!/usr/bin/env python3
"""Security and failure-path tests; no real credentials or external network."""
import contextlib
import importlib.machinery
import io
import json
import os
import pty
import select
import time
from pathlib import Path
import stat
import sys
import tempfile
import threading
import unittest
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from unittest.mock import patch

AI = Path(__file__).resolve().parents[1] / "dani-ai"
m = importlib.machinery.SourceFileLoader("daniai_regression", str(AI)).load_module()


class TTY(io.StringIO):
    def isatty(self):
        return True


class Tests(unittest.TestCase):
    def stream(self, data, out=None, live=None):
        out = out if out is not None else io.StringIO()
        with patch.object(m, "post_json", return_value=io.BytesIO(data)):
            return m.request_stream("http://local", "fake", "m", [], 1,
                                    None, None, out, live)

    def test_stream_rejects_truncation_and_errors(self):
        for data in (b'', b'data: invalid\n\n',
                     b'data: {"error":{"message":"failed"}}\n\n',
                     b'data: {"choices":[{"delta":{"content":"partial"}}]}\n\n',
                     b'data: {"choices":[{"delta":{"content":42}}]}\n\n'):
            with self.subTest(data=data), self.assertRaises(m.AIError):
                self.stream(data)

    def test_stream_accepts_metadata_usage_and_done(self):
        data = (b': keepalive\n\ndata: {"choices":[{"delta":{"role":"assistant"}}]}\n\n'
                b'data: {"choices":[{"delta":{"content":"ok"}}]}\n\n'
                b'data: {"choices":[],"usage":{}}\n\ndata: [DONE]\n\n')
        self.assertEqual(self.stream(data), 'ok')

    def test_tty_stream_is_immediate_and_sanitized_across_chunks(self):
        out = TTY()
        chunks = ['hello', '\x1b]52;c;BAD', '\x07world']
        def lines():
            for i, text in enumerate(chunks):
                if i == 1:
                    self.assertIn('hello', out.getvalue())
                yield ('data: ' + json.dumps({'choices': [{'delta': {'content': text}}]}) + '\n').encode()
            yield b'data: [DONE]\n'
        response = unittest.mock.MagicMock()
        response.__enter__.return_value = lines()
        with patch.object(m, 'post_json', return_value=response):
            raw = m.request_stream('x', 'k', 'm', [], 1, None, None, out)
        self.assertIn('\x1b]', raw)
        self.assertEqual(out.getvalue(), 'helloworld\n')

    def test_stream_markdown_across_every_chunk_boundary(self):
        text = '## Title\n**bold** and `code`\n```python\nx = 1\n```\n- item\n> quote'
        for split in range(len(text) + 1):
            chunks = [text[:split], text[split:]]
            data = b''.join(('data: ' + json.dumps({'choices': [{'delta': {'content': chunk}}]}) + '\n').encode()
                            for chunk in chunks) + b'data: [DONE]\n'
            out = TTY()
            with self.subTest(split=split):
                self.assertEqual(self.stream(data, out, m._MdLive()), text)
                self.assertEqual(out.getvalue(), m.render_markdown(text) + '\n')

    def test_stream_markdown_lines_visible_before_done(self):
        out = TTY()
        def lines():
            for chunk in ['**bo', 'ld**\n', '`tail', '`']:
                yield ('data: ' + json.dumps({'choices': [{'delta': {'content': chunk}}]}) + '\n').encode()
            self.assertEqual(out.getvalue(), m.render_markdown('**bold**') + '\n')
            yield b'data: [DONE]\n'
        response = unittest.mock.MagicMock()
        response.__enter__.return_value = lines()
        with patch.object(m, 'post_json', return_value=response):
            m.request_stream('x', 'k', 'm', [], 1, None, None, out, m._MdLive())
        self.assertEqual(out.getvalue(), m.render_markdown('**bold**\n`tail`') + '\n')

    def test_ctrl_v_in_real_readline(self):
        pid, fd = pty.fork()
        if pid == 0:
            code = ("import importlib.machinery; "
                    "m=importlib.machinery.SourceFileLoader('m', %r).load_module(); "
                    "m.read_clipboard=lambda: ('image','data:image/png;base64,aGVsbG8='); "
                    "print('RESULT',repr(m.input_boxed(False)),repr(m._INPUT_IMAGES)); "
                    "m.read_clipboard=lambda: ('text','pasted text'); "
                    "print('TEXT',repr(m.input_boxed(False)))") % str(AI)
            os.execvp(sys.executable, [sys.executable, '-c', code])
        output = b''
        sent = 0
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if not select.select([fd], [], [], 0.1)[0]:
                    continue
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                output += chunk
                if sent == 0 and b'> ' in output:
                    os.write(fd, b'question \x16\r')
                    sent = 1
                elif sent == 1 and output.count(b'> ') >= 2:
                    os.write(fd, b'\x16\r')
                    sent = 2
            else:
                self.fail('readline hung: %r' % output)
            _, status = os.waitpid(pid, 0)
            pid = 0
            self.assertEqual(status, 0, output)
            self.assertIn("RESULT 'question [Image 1]'".encode(), output)
            self.assertIn(b"TEXT 'pasted text'", output)
        finally:
            os.close(fd)
            if pid:
                os.kill(pid, 9)
                os.waitpid(pid, 0)

    def test_terminal_multiline_paste_waits_for_enter(self):
        pid, fd = pty.fork()
        if pid == 0:
            code = ("import importlib.machinery; "
                    "m=importlib.machinery.SourceFileLoader('m', %r).load_module(); "
                    "print('RESULT',repr(m.input_boxed(False)),flush=True)") % str(AI)
            os.execvp(sys.executable, [sys.executable, '-c', code])
        output = b''
        def collect_until(needle, seconds=5):
            nonlocal output
            deadline = time.monotonic() + seconds
            while needle not in output and time.monotonic() < deadline:
                if select.select([fd], [], [], 0.05)[0]:
                    try:
                        chunk = os.read(fd, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    output += chunk
            self.assertIn(needle, output)
        try:
            collect_until(b'> ')
            self.assertIn(b'\x1b[?2004h', output)
            # Simulate a terminal's Ctrl+Shift+V including trailing newlines.
            os.write(fd, b'\x1b[200~first line\nsecond line\n\x1b[201~')
            deadline = time.monotonic() + 0.3
            while time.monotonic() < deadline:
                if select.select([fd], [], [], 0.05)[0]:
                    output += os.read(fd, 65536)
            self.assertNotIn(b'RESULT', output, 'paste submitted without Enter')
            # Still editable: append text, then explicitly submit.
            os.write(fd, b'edited\r')
            collect_until(b'RESULT')
            collect_until(b"first line\\nsecond line\\nedited")
            _, status = os.waitpid(pid, 0)
            pid = 0
            self.assertEqual(status, 0, output)
        finally:
            os.close(fd)
            if pid:
                os.kill(pid, 9)
                os.waitpid(pid, 0)

    def test_clipboard_image_and_text(self):
        png = b'\x89PNG\r\n\x1a\nexample'
        with patch.dict(os.environ, {'DISPLAY': ':0', 'WAYLAND_DISPLAY': ''}), \
             patch.object(m.shutil, 'which', return_value='/usr/bin/xclip'), \
             patch.object(m, 'clipboard_read_command', side_effect=[b'image/png\nUTF8_STRING\n', png]) as read:
            kind, url = m.read_clipboard()
            self.assertEqual(kind, 'image')
            self.assertTrue(url.startswith('data:image/png;base64,'))
            self.assertIn('image/png', read.call_args.args[0])
        with patch.dict(os.environ, {'WAYLAND_DISPLAY': 'wayland-0'}), \
             patch.object(m.shutil, 'which', return_value='/usr/bin/wl-paste'), \
             patch.object(m, 'clipboard_read_command', side_effect=[b'text/plain\n', b'hello\x1b[2J']):
            self.assertEqual(m.read_clipboard(), ('text', 'hello'))
        with patch.dict(os.environ, {'DISPLAY': ':0', 'WAYLAND_DISPLAY': ''}), \
             patch.object(m.shutil, 'which', return_value='xclip'), \
             patch.object(m, 'clipboard_read_command', side_effect=[b'image/png', b'invalid']):
            with self.assertRaises(m.AIError):
                m.read_clipboard()

    def test_clipboard_limits_and_failure(self):
        self.assertEqual(m.clipboard_read_command([sys.executable, '-c', 'print("ok")']), b'ok\n')
        with self.assertRaises(m.AIError):
            m.clipboard_read_command([sys.executable, '-c', 'print("too long")'], 3)
        with self.assertRaises(m.AIError):
            m.clipboard_read_command([sys.executable, '-c', 'raise SystemExit(1)'])

    def test_image_attachment_and_session_roundtrip(self):
        url = 'data:image/png;base64,aGVsbG8='
        with patch.object(m, '_INPUT_IMAGES', [('[Image 1]', url)]):
            content = m.attached_content('describe [Image 1]')
            self.assertEqual(content, [{'type': 'text', 'text': 'describe'},
                                      {'type': 'image_url', 'image_url': {'url': url}}])
            self.assertEqual(m.attached_content('deleted'), 'deleted')
            self.assertEqual(len(m.attached_content('[Image 1]')), 1)
        self.assertTrue(m.valid_user_content(content))
        self.assertFalse(m.valid_user_content(content, 'system'))
        self.assertFalse(m.valid_user_content([{'type': 'image_url', 'image_url': {'url': 'file:///secret'}}]))
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, 'chat.json')
            messages = [{'role': 'user', 'content': content}]
            self.assertTrue(m.save_session(path, 'vision', messages))
            self.assertEqual(m.load_session(path), ('vision', messages))

    def test_http_and_bad_json_failures(self):
        import urllib.error
        import http.client
        request = urllib.request.Request('https://example.test/v1/models')
        opener = unittest.mock.Mock()
        for error in (urllib.error.HTTPError(request.full_url, 429, 'busy', {}, io.BytesIO(b'busy')),
                      urllib.error.URLError('offline'), TimeoutError('timeout')):
            opener.open.side_effect = error
            with patch.object(m.urllib.request, 'build_opener', return_value=opener):
                with self.assertRaises(m.AIError):
                    m.open_request(request, 1)
        for raw in (b'bad json', b'{}', b'{"choices":[{"message":{"content":42}}]}'):
            with patch.object(m, 'post_json', return_value=io.BytesIO(raw)):
                with self.assertRaises(m.AIError):
                    m.request_once('x', 'fake', 'm', [], 1, None, None)
        with patch.object(m, 'post_json', side_effect=http.client.IncompleteRead(b'part')):
            with self.assertRaises(http.client.IncompleteRead):
                m.request_once('x', 'fake', 'm', [], 1, None, None)

    def test_terminal_controls(self):
        self.assertEqual(m.safe_terminal('a\x1b[2Jb\x1b]52;c;secret\x07c\r\x08\u202ed'), 'abcd')
        out = TTY()
        with patch.object(m, 'request_once', return_value='a\x1b[2Jb'):
            m.complete('x', 'k', 'm', [], 1, None, None, False, out)
        self.assertTrue(out.getvalue().endswith('\rab\n'))
        self.assertNotIn('\x1b', out.getvalue())

    def test_private_atomic_session_and_history(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'history' / 'last.json'
            self.assertTrue(m.save_session(str(path), 'm', []))
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
            old = path.read_bytes()
            with patch.object(m.os, 'replace', side_effect=OSError('disk failure')):
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertFalse(m.save_session(str(path), 'new', []))
            self.assertEqual(path.read_bytes(), old)
            self.assertEqual(list(path.parent.iterdir()), [path])
            history = path.parent / '.repl_history'
            m.atomic_private_write(str(history), lambda p: Path(p).write_text('private'))
            self.assertEqual(stat.S_IMODE(history.stat().st_mode), 0o600)

    def test_invalid_sessions(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'bad.json'
            for obj in ([], None, {'messages': 'oops'}, {'messages': [{}]},
                        {'model': [], 'messages': []},
                        {'messages': [{'role': 'user', 'content': 1}]}):
                path.write_text(json.dumps(obj))
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(m.load_session(str(path)), (None, None))

    def test_single_shot_continue(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'daniwm/ai-history/last.json'
            old = [{'role': 'user', 'content': 'old'}, {'role': 'assistant', 'content': 'answer'}]
            m.save_session(str(path), 'm', old)
            with patch.dict(os.environ, {'XDG_CONFIG_HOME': d, 'DANI_AI_API_KEY': 'fake'}, clear=True), \
                 patch.object(m, 'read_piped_stdin', return_value=''), \
                 patch.object(m, 'complete', return_value='new') as complete:
                self.assertEqual(m.main(['--continue', 'last', 'next']), 0)
                self.assertEqual(complete.call_args.args[3], old + [{'role': 'user', 'content': 'next'}])

    def test_repl_survives_errors_cancels_and_saves(self):
        with tempfile.TemporaryDirectory() as d:
            effects = ['first answer', m.AIError('HTTP 429'), TimeoutError('timeout'), KeyboardInterrupt()]
            with patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=['first', 'failed', 'timeout', 'cancel', '/q']), \
                 patch.object(m, 'complete', side_effect=effects), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                m.repl('x', 'fake', 'm', '', 1, None, None, True, False, d)
            _, messages = m.load_session(str(Path(d) / 'last.json'))
            self.assertEqual([x['content'] for x in messages], ['first', 'first answer'])

    def test_repl_finally_saves_on_unexpected_exit(self):
        with tempfile.TemporaryDirectory() as d:
            with patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=['hello', SystemExit(1)]), \
                 patch.object(m, 'complete', return_value='answer'), \
                 contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d)
            self.assertTrue((Path(d) / 'last.json').exists())

    def test_clear_terminal_and_new_session(self):
        for tty in (True, False):
            out = TTY() if tty else io.StringIO()
            with self.subTest(tty=tty), tempfile.TemporaryDirectory() as d, \
                 patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=['/clear', 'hello', '/q']), \
                 patch.object(m, 'complete', return_value='answer') as complete, \
                 contextlib.redirect_stdout(out):
                initial = ('model', [{'role': 'system', 'content': 'instructions'},
                                     {'role': 'user', 'content': 'old question'},
                                     {'role': 'assistant', 'content': 'old answer'}])
                m.repl('x', 'fake', 'model', '', 1, None, None, False, False, d, initial)
                sent = complete.call_args.args[3]
                self.assertEqual(sent[:2], [{'role': 'system', 'content': 'instructions'},
                                           {'role': 'user', 'content': 'hello'}])
                self.assertFalse(any(msg['content'] == 'old question' for msg in sent))
                self.assertIn('(new session)', out.getvalue())
                self.assertEqual('\x1b[2J\x1b[3J\x1b[H' in out.getvalue(), tty)

    def test_exact_commands(self):
        with tempfile.TemporaryDirectory() as d, \
             patch.object(m, 'setup_readline', return_value=''), \
             patch.object(m, 'input_boxed', side_effect=['/systematic', '/baseline', '/streaming', '/colorful', '/q']), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d)
            self.assertEqual(out.getvalue().count('unknown command'), 4)

    def test_waiting_nonstream_cleans_up_before_answer(self):
        out = TTY()
        def response(*args):
            self.assertIn('Waiting for response', out.getvalue())
            return 'answer'
        with patch.object(m, 'request_once', side_effect=response):
            m.complete('x', 'fake', 'm', [], 1, None, None, False, out)
        self.assertTrue(out.getvalue().endswith('\ranswer\n'))
        self.assertNotIn('\x1b', out.getvalue())

    def test_waiting_stream_stops_on_first_content(self):
        out = TTY()
        response = unittest.mock.MagicMock()
        def lines():
            self.assertIn('Waiting for response', out.getvalue())
            yield b'data: {"choices":[{"delta":{"content":"hello"}}]}\n'
            self.assertTrue(out.getvalue().endswith('hello'))
            yield b'data: [DONE]\n'
        response.__enter__.return_value = lines()
        with patch.object(m, 'post_json', return_value=response):
            m.complete('x', 'fake', 'm', [], 1, None, None, True, out)
        self.assertTrue(out.getvalue().endswith('hello\n'))

    def test_waiting_cleanup_errors_and_pipes(self):
        for error in (m.AIError('failed'), KeyboardInterrupt()):
            out = TTY()
            with patch.object(m, 'request_once', side_effect=error):
                with self.assertRaises(type(error)):
                    m.complete('x', 'fake', 'm', [], 1, None, None, False, out)
            self.assertTrue(out.getvalue().endswith(' ' * 28 + '\r'))
        out = io.StringIO()
        with patch.object(m, 'request_once', return_value='answer'):
            m.complete('x', 'fake', 'm', [], 1, None, None, False, out)
        self.assertEqual(out.getvalue(), 'answer\n')
        with m._Waiting(io.StringIO()) as waiting:
            self.assertIsNone(waiting.thread)

    def test_tools_read_blocklist_and_boundaries(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'
            root.mkdir()
            (root / 'readme.txt').write_text('hello\nworld')
            (root / '.env').write_text('secret')
            (root / 'ai.key').write_text('secret')
            (root / 'link').symlink_to(root / 'readme.txt')
            runner = m.ToolRunner(str(root))
            self.assertEqual(len(runner.schemas), 5)
            self.assertEqual(json.loads(runner.execute('read_file', '{"path":"readme.txt"}'))['result'], 'hello\nworld')
            self.assertIn('2: world', runner.execute('search_text', '{"path":"readme.txt","query":"world"}'))
            for path in ('../outside', '/etc/passwd', '.env', 'ai.key', 'link'):
                self.assertIn('error', json.loads(runner.execute('read_file', json.dumps({'path': path}))))
            blocked = root / 'blocklist'
            blocked.write_text('# comment\nread_*\nsearch_text\n')
            runner = m.ToolRunner(str(root), ['list_files', 'run_command', 'write_file', 'edit_file'], str(blocked))
            self.assertEqual(runner.schemas, [])
            self.assertIn('error', json.loads(runner.execute('read_file', '{"path":"readme.txt"}')))
            self.assertIn('error', json.loads(runner.execute('shell', '{}')))

    def test_file_writes_and_edits(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            runner = m.ToolRunner(d)
            def execute(name, **args):
                return json.loads(runner.execute(name, json.dumps(args)))
            result = execute('write_file', path='hello.txt', content='hello\nworld\n')
            self.assertEqual(result['result']['action'], 'created')
            self.assertEqual(stat.S_IMODE((root / 'hello.txt').stat().st_mode), 0o600)
            self.assertIn('error', execute('write_file', path='hello.txt', content='overwrite'))
            os.chmod(root / 'hello.txt', 0o755)
            result = execute('edit_file', path='hello.txt', old_text='world', new_text='Việt Nam')
            self.assertEqual(result['result']['action'], 'edited')
            self.assertEqual((root / 'hello.txt').read_text(), 'hello\nViệt Nam\n')
            self.assertEqual(stat.S_IMODE((root / 'hello.txt').stat().st_mode), 0o755)
            for old in ('missing', '', 'l'):
                self.assertIn('error', execute('edit_file', path='hello.txt', old_text=old, new_text='x'))
            (root / 'sub').mkdir()
            self.assertIn('result', execute('write_file', path='sub/new.txt', content='nested'))
            self.assertIn('error', execute('write_file', path='missing/new.txt', content='x'))
            self.assertIn('error', execute('write_file', path='big.txt', content='é' * 32769))
            self.assertIn('error', execute('edit_file', path='hello.txt', old_text='hello', new_text='x' * 65536))
            self.assertIn('error', execute('write_file', path='nul.txt', content='\x00'))
            for path in ('../escape', '/tmp/escape', '.env', 'ai.key', 'sub/.git/config', '', '.'):
                self.assertIn('error', execute('write_file', path=path, content='x'))
                self.assertIn('error', execute('edit_file', path=path, old_text='a', new_text='b'))
            (root / 'link').symlink_to(root / 'hello.txt')
            (root / 'dirlink').symlink_to(root / 'sub', target_is_directory=True)
            for path in ('link', 'dirlink/new.txt'):
                self.assertIn('error', execute('write_file', path=path, content='x'))
                self.assertIn('error', execute('edit_file', path=path, old_text='hello', new_text='x'))
            os.link(root / 'hello.txt', root / 'hardlink')
            self.assertIn('error', execute('edit_file', path='hardlink', old_text='hello', new_text='x'))
            os.mkfifo(root / 'fifo')
            self.assertIn('error', execute('edit_file', path='fifo', old_text='a', new_text='b'))
            (root / 'binary').write_bytes(b'\x00data')
            self.assertIn('error', execute('edit_file', path='binary', old_text='data', new_text='x'))
            blocked = m.ToolRunner(d, ['write_file', 'edit_file'])
            for name, args in (('write_file', {'path': 'blocked', 'content': 'x'}),
                               ('edit_file', {'path': 'hello.txt', 'old_text': 'hello', 'new_text': 'x'})):
                self.assertIn('error', json.loads(blocked.execute(name, json.dumps(args))))
            self.assertFalse(list(root.glob('.dani-ai-write-*')))

    def test_linux_commands(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d, allow_shell=True)
            result = json.loads(runner.execute('run_command', json.dumps({
                'command': 'printf hello > output.txt; cat output.txt; printf error >&2; exit 7'})))
            self.assertEqual(result['output'], 'helloerror')
            self.assertEqual(result['exit_code'], 7)
            self.assertEqual((Path(d) / 'output.txt').read_text(), 'hello')
            self.assertIsNone(result['stopped'])
            runner = m.ToolRunner(d, ['run_*'])
            self.assertIn('error', json.loads(runner.execute('run_command', '{"command":"touch blocked"}')))
            self.assertFalse((Path(d) / 'blocked').exists())

    def test_shell_requires_opt_in(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            self.assertNotIn('run_command', [s['function']['name'] for s in runner.schemas])
            result = json.loads(runner.execute('run_command', '{"command":"touch blocked"}'))
            self.assertIn('error', result)
            self.assertFalse((Path(d) / 'blocked').exists())
            # Explicit blocklists take precedence even after opting in.
            runner = m.ToolRunner(d, ['run_*'], allow_shell=True)
            self.assertIn('error', json.loads(runner.execute('run_command', '{"command":"true"}')))
        with tempfile.TemporaryDirectory() as config, patch.dict(os.environ, {'XDG_CONFIG_HOME': config}):
            for flags, enabled in (([], False), (['--allow-shell'], True),
                                   (['--yes-tools'], False),
                                   (['--allow-shell', '--block-tool', 'run_command'], False)):
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    self.assertEqual(m.main(flags + ['tools']), 0)
                self.assertEqual('run_command' in out.getvalue(), enabled)

    def test_custom_large_json_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            manifest = Path(d) / 'large.json'
            manifest.write_text(json.dumps({
                'name': 'large_json', 'description': 'large result',
                'command': [sys.executable, '-c',
                            'import json; print(json.dumps({"candles": ["x"*100]*200, "tail": "kept"}))'],
                'output_limit': 131072}))
            result = json.loads(m.CustomTool(manifest).run({}, d))
            self.assertIsInstance(result['result'], dict)
            self.assertEqual(len(result['result']['candles']), 200)
            self.assertEqual(result['result']['tail'], 'kept')

    def test_incomplete_response_rejected(self):
        for reason in ('length', 'content_filter'):
            for tools in (None, []):
                response = {'choices': [{'message': {'content': 'partial'}, 'finish_reason': reason}]}
                with patch.object(m, 'post_json', return_value=io.BytesIO(json.dumps(response).encode())):
                    with self.assertRaises(m.AIError):
                        m.request_once('x', 'fake', 'm', [], 1, None, None, tools)
                chunks = [
                    {'choices': [{'delta': {'content': 'partial'}}]},
                    {'choices': [{'delta': {}, 'finish_reason': reason}]}]
                data = b''.join(('data: ' + json.dumps(c) + '\n\n').encode() for c in chunks)
                data += b'data: [DONE]\n\n'
                with patch.object(m, 'post_json', return_value=io.BytesIO(data)):
                    with self.assertRaises(m.AIError):
                        m.request_stream('x', 'fake', 'm', [], 1, None, None, io.StringIO(), tools=tools)

    def test_gold_proxy_tool(self):
        gold = importlib.machinery.SourceFileLoader(
            'gold_regression', str(AI.parent / 'ai.tools/xauusd_market.py')).load_module()
        fixture = {'chart': {'error': None, 'result': [{
            'meta': {'symbol': 'GC=F', 'currency': 'USD', 'regularMarketTime': 900,
                     'regularMarketPrice': 2500, 'exchangeTimezoneName': 'America/New_York'},
            'timestamp': [100, 200, 300],
            'indicators': {'quote': [{'open': [2400, None, 2490],
                                      'high': [2450, None, 2510],
                                      'low': [2390, None, 2480],
                                      'close': [2440, None, 2500], 'volume': [10, None, 20]}]}}]}}
        result = gold.parse_chart(fixture, '1h', 1, now=1000)
        self.assertEqual(result['symbol'], 'GC=F')
        self.assertIn('NOT XAUUSD spot', result['instrument'])
        self.assertEqual(result['quote']['age_seconds'], 100)
        self.assertEqual(result['klines']['count'], 1)
        self.assertEqual(result['klines']['candles'][0]['t'], 300)
        self.assertEqual(gold.parse_chart(fixture, '1h', 200)['klines']['count'], 2)
        manifest = m.CustomTool(AI.parent / 'ai.tools/xauusd_market.json')
        self.assertIsNone(manifest.validate({'interval': '1h', 'limit': 200}))
        self.assertIsNotNone(manifest.validate({'interval': '4h'}))
        for args in ({}, {'interval': '1d', 'limit': 1}):
            out = io.StringIO()
            with patch.object(gold, 'fetch', return_value=fixture), \
                 patch('sys.stdin', io.StringIO(json.dumps(args))), contextlib.redirect_stdout(out):
                self.assertEqual(gold.main(), 0)
            self.assertEqual(json.loads(out.getvalue())['source'], 'yahoo-finance-chart')
        for args in ([], {'limit': True}, {'limit': 201}, {'interval': []}, {'symbol': 'XAUUSD'}):
            with patch.object(gold, 'fetch') as fetch, \
                 patch('sys.stdin', io.StringIO(json.dumps(args))), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gold.main(), 1)
                fetch.assert_not_called()
        for payload in ({}, {'chart': {'error': {'code': 'Bad Request'}}},
                        {'chart': {'result': []}}):
            out = io.StringIO()
            with patch.object(gold, 'fetch', return_value=payload), \
                 patch('sys.stdin', io.StringIO('{}')), contextlib.redirect_stdout(out):
                self.assertEqual(gold.main(), 1)
            self.assertIn('error', json.loads(out.getvalue()))

    def test_btc_ratio_units_and_periods(self):
        btc = importlib.machinery.SourceFileLoader(
            'btc_audit_regression', str(AI.parent / 'ai.tools/btc_market.py')).load_module()
        for interval in sorted(btc.INTERVALS):
            out = io.StringIO()
            with patch.object(btc, 'get', return_value=[{
                    'longAccount': '0.62', 'shortAccount': '0.38',
                    'longShortRatio': '1.63', 'timestamp': 123}]) as get, \
                 patch('sys.stdin', io.StringIO(json.dumps({
                     'include': ['long_short'], 'interval': interval}))), \
                 contextlib.redirect_stdout(out):
                self.assertEqual(btc.main(), 0)
            result = json.loads(out.getvalue())['long_short']
            period = get.call_args.args[1]['period']
            self.assertIn(period, {'5m', '15m', '30m', '1h', '2h', '4h', '6h', '12h', '1d'})
            self.assertEqual(period, {'1m': '5m', '8h': '6h', '1w': '1d'}.get(interval, interval))
            self.assertEqual(result['period'], period)
            self.assertAlmostEqual(result['latest']['long_pct'], 62)
            self.assertAlmostEqual(result['latest']['short_pct'], 38)
        # Missing values stay null, rather than being reported as zero percent.
        out = io.StringIO()
        with patch.object(btc, 'get', return_value=[]), \
             patch('sys.stdin', io.StringIO('{"include":["long_short"]}')), \
             contextlib.redirect_stdout(out):
            btc.main()
        self.assertIsNone(json.loads(out.getvalue())['long_short']['latest']['long_pct'])

    def test_command_limits_and_environment(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            result = json.loads(runner.run_command('sleep 10', timeout=0.05))
            self.assertEqual(result['stopped'], 'timeout')
            result = json.loads(runner.run_command('yes output', output_limit=100))
            self.assertEqual(result['stopped'], 'output_limit')
            self.assertEqual(len(result['output']), 100)
            with patch.dict(os.environ, {'DANI_AI_API_KEY': 'fake-secret'}):
                result = json.loads(runner.run_command('printf "%s" "$DANI_AI_API_KEY"'))
            self.assertEqual(result['output'], '')
            with self.assertRaises(ValueError):
                runner.run_command('')

    def test_custom_tools_manifest_and_execution(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'
            root.mkdir()
            tools = Path(d) / 'tools'
            tools.mkdir()
            script = tools / 'echo_args.py'
            script.write_text('import json,sys\n'
                              'args=json.load(sys.stdin)\n'
                              'print(json.dumps({"got": args, "root": sys.argv[1]}))\n')
            manifest = {'name': 'echo_args', 'description': 'Echo arguments back',
                        'command': [sys.executable, '{manifest_dir}/echo_args.py', 'fixed'],
                        'parameters': {'type': 'object',
                                       'properties': {'text': {'type': 'string', 'maxLength': 8},
                                                      'mode': {'type': 'string', 'enum': ['a', 'b']}},
                                       'required': ['text'], 'additionalProperties': False}}
            (tools / 'echo_args.json').write_text(json.dumps(manifest))
            # Invalid manifest (bad name) is skipped, not fatal.
            (tools / 'bad.json').write_text(json.dumps({'name': 'Bad Name', 'description': 'x',
                                                        'command': ['true']}))
            # Duplicate of a builtin name is rejected.
            (tools / 'dup.json').write_text(json.dumps({'name': 'read_file', 'description': 'x',
                                                        'command': ['true']}))
            runner = m.ToolRunner(str(root), custom_dir=str(tools))
            names = [s['function']['name'] for s in runner.schemas]
            self.assertIn('echo_args', names)
            self.assertEqual(names.count('read_file'), 1)
            self.assertEqual(len(runner.custom_errors), 2)
            result = json.loads(runner.execute('echo_args', json.dumps({'text': 'hi'})))
            self.assertEqual(result['result'], {'got': {'text': 'hi'}, 'root': 'fixed'})
            for bad in ('{}', '{"text":"toolongvalue"}', '{"text":"a","mode":"z"}',
                        '{"text":"a","extra":1}', 'not json', '[1]'):
                with self.subTest(bad=bad):
                    self.assertIn('error', json.loads(runner.execute('echo_args', bad)))
            self.assertEqual(runner.execute('echo_args', '{"text":"a"}').count('untrusted'), 1)

    def test_custom_tools_confirm_block_timeout_and_failures(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'
            root.mkdir()
            tools = Path(d) / 'tools'
            tools.mkdir()
            def manifest(name, body, **extra):
                obj = {'name': name, 'description': 'test tool %s' % name,
                       'command': [sys.executable, '-c', body]}
                obj.update(extra)
                (tools / (name + '.json')).write_text(json.dumps(obj))
            manifest('needs_ok', 'import sys; sys.stdin.read(); print("ran")', confirm=True)
            manifest('slow', 'import time; time.sleep(5)', timeout=0.2)
            manifest('fails', 'import sys; sys.stderr.write("boom"); sys.exit(3)')
            manifest('noisy', 'print("x" * 5000)', output_limit=256)
            asked = []
            def confirm(name, description):
                asked.append(name)
                return name == 'needs_ok'
            runner = m.ToolRunner(str(root), custom_dir=str(tools), confirm=confirm)
            self.assertEqual(json.loads(runner.execute('needs_ok', '{}'))['result'], 'ran\n')
            self.assertEqual(asked, ['needs_ok'])
            runner = m.ToolRunner(str(root), custom_dir=str(tools), confirm=lambda n, s: False)
            self.assertIn('declined', json.loads(runner.execute('needs_ok', '{}'))['error'])
            self.assertIn('timeout', json.loads(runner.execute('slow', '{}'))['error'])
            failure = json.loads(runner.execute('fails', '{}'))
            self.assertIn('exited with 3', failure['error'])
            self.assertIn('boom', failure['stderr'])
            self.assertIn('output_limit', json.loads(runner.execute('noisy', '{}'))['error'])
            blocked = m.ToolRunner(str(root), ['needs_ok'], custom_dir=str(tools))
            self.assertNotIn('needs_ok', [s['function']['name'] for s in blocked.schemas])
            self.assertIn('blocked', json.loads(blocked.execute('needs_ok', '{}'))['error'])
            missing = m.ToolRunner(str(root), custom_dir=str(Path(d) / 'absent'))
            self.assertEqual(missing.custom, {})
            self.assertEqual(len(missing.schemas), 5)

    def test_custom_tool_argument_schema_validation(self):
        schema = {'type': 'object', 'properties': {
            'n': {'type': 'integer', 'minimum': 1, 'maximum': 5},
            'f': {'type': 'number'},
            'tags': {'type': 'array', 'items': {'type': 'string'}, 'maxItems': 2},
            'flag': {'type': 'boolean'},
            'maybe': {'type': ['string', 'null']}},
            'required': ['n'], 'additionalProperties': False}
        cases = [({'n': 3}, True), ({'n': 3, 'f': 1.5, 'flag': False, 'maybe': None}, True),
                 ({'n': 3, 'tags': ['a']}, True), ({}, False), ({'n': 0}, False),
                 ({'n': 6}, False), ({'n': 3, 'f': True}, False), ({'n': 3, 'f': 'x'}, False),
                 ({'n': 3, 'tags': ['a', 'b', 'c']}, False), ({'n': 3, 'tags': [1]}, False),
                 ({'n': 3, 'flag': 'yes'}, False), ({'n': 3, 'maybe': 1}, False),
                 ({'n': 3, 'extra': 1}, False)]
        for value, valid in cases:
            with self.subTest(value=value):
                self.assertEqual(m.validate_json_value(value, schema) is None, valid)

    def test_tool_loop_and_history_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            (Path(d) / 'a').write_text('hello')
            messages = [{'role': 'user', 'content': 'read a'}]
            call = {'role': 'assistant', 'content': None, 'tool_calls': [
                {'id': 'c1', 'type': 'function', 'function': {'name': 'read_file', 'arguments': '{"path":"a"}'}}]}
            final = {'role': 'assistant', 'content': 'done'}
            with patch.object(m, 'request_once', side_effect=[call, final]) as request:
                result = m.complete('x', 'fake', 'm', messages, 1, None, None, False, io.StringIO(), runner=runner)
                self.assertEqual(request.call_count, 2)
            self.assertEqual(result, 'done')
            self.assertEqual(messages[-1]['role'], 'tool')
            messages.append(final)
            path = str(Path(d) / 'session.json')
            m.save_session(path, 'm', messages)
            self.assertEqual(m.load_session(path)[1], messages)
            before = list(messages)
            with patch.object(m, 'request_once', side_effect=[call, m.AIError('failed')]):
                with self.assertRaises(m.AIError):
                    m.complete('x', 'fake', 'm', messages, 1, None, None, False, io.StringIO(), runner=runner)
            self.assertEqual(messages, before)

    def test_ai_session_name(self):
        messages = [{'role': 'system', 'content': 'private system'},
                    {'role': 'user', 'content': [{'type': 'text', 'text': 'fix tool UI'},
                     {'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,AAAA'}}]},
                    {'role': 'tool', 'content': 'private tool output'}]
        with patch.object(m, 'request_once', return_value='Fix Tool UI!') as request:
            self.assertEqual(m.suggest_session_name('x', 'key', 'model', messages, 60), 'fix-tool-ui')
        prompt = str(request.call_args.args[3])
        self.assertNotIn('private system', prompt)
        self.assertNotIn('private tool output', prompt)
        self.assertNotIn('base64', prompt)
        self.assertEqual(request.call_args.args[4], 15)
        with patch.object(m, 'request_once', side_effect=m.AIError('offline')):
            self.assertRegex(m.suggest_session_name('x', 'key', 'model', messages, 60), r'^\d{8}-\d{6}$')
        with patch.object(m, 'request_once') as request:
            m.suggest_session_name('x', 'key', 'model', [], 60)
            request.assert_not_called()

    def test_save_auto_title_avoids_overwrite(self):
        with tempfile.TemporaryDirectory() as d, \
             patch.object(m, 'setup_readline', return_value=''), \
             patch.object(m, 'input_boxed', side_effect=['hello', '/save', '/save', '/save manual', '/q']), \
             patch.object(m, 'complete', return_value='answer'), \
             patch.object(m, 'suggest_session_name', return_value='hello-world') as title, \
             contextlib.redirect_stdout(io.StringIO()):
            m.repl('x', 'fake', 'model', '', 1, None, None, False, False, d)
            self.assertEqual(title.call_count, 2)
            for name in ('hello-world', 'hello-world-2', 'manual'):
                self.assertTrue((Path(d) / (name + '.json')).is_file())

    def test_resume_command_and_picker(self):
        for command, picks in [('/resume saved', []), ('/resume', ['1']), ('/resume last', [])]:
            with tempfile.TemporaryDirectory() as d:
                saved = [{'role': 'system', 'content': 'saved system'},
                         {'role': 'user', 'content': 'old question'},
                         {'role': 'assistant', 'content': 'old answer'}]
                name = 'last' if command.endswith(' last') else 'saved'
                m.save_session(os.path.join(d, name + '.json'), 'saved-model', saved)
                with patch.object(m, 'setup_readline', return_value=''), \
                     patch.object(m, 'input_boxed', side_effect=[command, 'next question', '/q']), \
                     patch('builtins.input', side_effect=picks), \
                     patch.object(m, 'complete', return_value='next answer') as complete, \
                     contextlib.redirect_stdout(io.StringIO()) as out:
                    m.repl('x', 'fake', 'original', 'original system', 1, None, None, False, False, d)
                self.assertIn('resumed ' + name, out.getvalue())
                self.assertEqual(complete.call_args.args[2], 'saved-model')
                self.assertEqual(complete.call_args.args[3][:3], saved)
                self.assertEqual(m.complete_token(d, '/resume sa', 'sa', 0),
                                 'saved' if name == 'saved' else None)

    def test_resume_failure_keeps_current_session(self):
        with tempfile.TemporaryDirectory() as d, \
             patch.object(m, 'setup_readline', return_value=''), \
             patch.object(m, 'input_boxed', side_effect=['/resume ../bad', '/resume missing', 'hello', '/q']), \
             patch.object(m, 'complete', return_value='answer') as complete, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            m.repl('x', 'fake', 'original', 'original system', 1, None, None, False, False, d)
            self.assertEqual(complete.call_args.args[2], 'original')
            self.assertEqual(complete.call_args.args[3][0]['content'], 'original system')

    def test_stream_frame_skips_empty_tool_responses(self):
        for chunks in ([], [''], ['\n', '  ', '\t']):
            out = TTY()
            frame = m._FrameWriter(out, 'model')
            self.assertEqual(out.getvalue(), '')
            for chunk in chunks:
                frame.write(chunk)
            frame.end()
            self.assertEqual(out.getvalue(), '')

    def test_answer_body_is_copy_safe(self):
        text = '• First item\n• Second item\n\nParagraph.\n  indented code\n'
        framed = m.safe_terminal(m.frame_answer(text, 'model', width=40))
        body = framed.split('\n', 1)[1].rsplit('\n', 1)[0]
        self.assertEqual(body, text)
        for split in range(len(text) + 1):
            out = io.StringIO()
            frame = m._FrameWriter(out, 'model', width=40)
            frame.write(text[:split])
            frame.write(text[split:])
            frame.end()
            plain = m.safe_terminal(out.getvalue())
            body = plain.split('\n', 1)[1].rsplit('\n', 2)[0]
            self.assertEqual(body, text.rstrip('\n'))
            self.assertNotIn('│', plain)
        # Actual user/model characters must not be stripped or rewritten.
        self.assertEqual(m._frame_line('│ actual content'), '│ actual content')

    def test_stream_frame_opens_on_first_text(self):
        out = TTY()
        frame = m._FrameWriter(out, 'model')
        frame.write('\n')
        self.assertEqual(out.getvalue(), '')
        frame.write('hello')
        self.assertIn('┌─ model', out.getvalue())
        self.assertIn('hello', out.getvalue())
        frame.write(' world')
        frame.end()
        self.assertEqual(out.getvalue().count('┌─ model'), 1)
        self.assertIn('hello world', out.getvalue())
        self.assertIn('└', out.getvalue())

    def test_tool_display_preview_and_status(self):
        out = TTY()
        function = {'name': 'run_command', 'arguments': '{"command":"ls"}'}
        with m._ToolDisplay(out, function) as display:
            display.finish(json.dumps({'output': '\n'.join('line%d' % i for i in range(9)),
                                       'exit_code': 7, 'stopped': None}))
        text = out.getvalue()
        self.assertIn('┌─ run_command', text)
        self.assertIn('command: ls', text)
        self.assertIn('Running…', text)
        self.assertIn('4 more lines', text)
        self.assertIn('✗ Error · exit 7', text)
        self.assertNotIn('line8', text)
        self.assertNotIn('\x1b', text)

    def test_tool_display_pipe_and_terminal_safety(self):
        function = {'name': 'read_file\x1b[2J', 'arguments': '{"path":"a\\nforged"}'}
        out = io.StringIO()
        with m._ToolDisplay(out, function) as display:
            display.finish('{"result":"hello"}')
        self.assertEqual(out.getvalue(), '')
        out = TTY()
        with patch.object(m.shutil, 'get_terminal_size', return_value=os.terminal_size((32, 24))):
            with m._ToolDisplay(out, function) as display:
                display.finish(json.dumps({'error': '\x1b[31m' + '界' * 100}))
        text = out.getvalue()
        self.assertNotIn('\x1b', text)
        self.assertIn('path: a forged', text)
        self.assertIn('✗ Error', text)
        for line in text.splitlines():
            self.assertLessEqual(sum(2 if m.unicodedata.east_asian_width(c) in 'WF' else 1
                                     for c in line), 31)

    def test_tool_display_exception_closes_card(self):
        out = TTY()
        with self.assertRaises(ValueError):
            with m._ToolDisplay(out, {'name': 'read_file', 'arguments': '{}'}):
                raise ValueError('failed')
        self.assertIn('failed', out.getvalue())
        self.assertIn('└─ ✗ Error', out.getvalue())

    def test_stream_tool_arguments_assembled(self):
        chunks = [
            {'tool_calls': [{'index': 0, 'id': 'c1', 'type': 'function', 'function': {'name': 'read_file', 'arguments': '{"pa'}}]},
            {'tool_calls': [{'index': 0, 'function': {'arguments': 'th":"a"}'}}]}]
        data = ''.join('data: ' + json.dumps({'choices': [{'delta': c}]}) + '\n\n' for c in chunks)
        with patch.object(m, 'post_json', return_value=io.BytesIO((data + 'data: [DONE]\n\n').encode())):
            result = m.request_stream('x', 'k', 'm', [], 1, None, None, io.StringIO(), tools=[{}])
        self.assertEqual(result['tool_calls'][0]['function']['arguments'], '{"path":"a"}')

    def test_no_color_input(self):
        with patch.object(m.sys, 'stdout', TTY()), patch('builtins.input', return_value='') as inp:
            m.input_boxed(False)
            self.assertNotIn('\x1b', inp.call_args.args[0])
        with patch.dict(os.environ, {'NO_COLOR': ''}), patch.object(m.sys, 'stdout', TTY()):
            self.assertFalse(m.color_enabled(True))

    def test_redirect_protection_local_servers(self):
        seen = []
        class Target(BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append(self.headers.get('Authorization'))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"data":[]}')
            def log_message(self, *args):
                pass
        target = HTTPServer(('127.0.0.1', 0), Target)
        class Redirect(Target):
            def do_GET(self):
                if self.path == '/safe':
                    self.send_response(302)
                    self.send_header('Location', '/done')
                    self.end_headers()
                elif self.path == '/done':
                    super().do_GET()
                else:
                    self.send_response(302)
                    self.send_header('Location', 'http://127.0.0.1:%d/models' % target.server_port)
                    self.end_headers()
        source = HTTPServer(('127.0.0.1', 0), Redirect)
        threads = []
        try:
            for server in (source, target):
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                threads.append(thread)
            base = 'http://127.0.0.1:%d' % source.server_port
            for method in ('GET', 'POST'):
                # Direct handler test covers redirect decisions for both methods.
                req = urllib.request.Request(base + '/models', method=method)
                with self.assertRaises(m.AIError):
                    m.SafeRedirectHandler().redirect_request(req, None, 302, '', {},
                        'http://127.0.0.1:%d/models' % target.server_port)
            with self.assertRaises(m.AIError):
                m.open_request(urllib.request.Request(base + '/models', headers={'Authorization': 'Bearer fake'}), 2)
            self.assertEqual(seen, [])
            with m.open_request(urllib.request.Request(base + '/safe', headers={'Authorization': 'Bearer fake'}), 2) as response:
                self.assertEqual(response.status, 200)
            self.assertEqual(seen, ['Bearer fake'])
            req = urllib.request.Request('https://example.com/v1/models')
            with self.assertRaises(m.AIError):
                m.SafeRedirectHandler().redirect_request(req, None, 302, '', {}, 'http://example.com/v1/models')
        finally:
            for server in (source, target):
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join()


if __name__ == '__main__':
    unittest.main()
