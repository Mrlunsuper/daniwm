#!/usr/bin/env python3
"""Security and failure-path tests; no real credentials or external network."""
import contextlib
import importlib.machinery
import io
import json
import os
from pathlib import Path
import stat
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
            raw = m.request_stream('x', 'k', 'm', [], 1, None, None, out, m._MdLive())
        self.assertIn('\x1b]', raw)
        self.assertEqual(out.getvalue(), 'helloworld\n')

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
            self.assertIn('Đang chờ phản hồi', out.getvalue())
            return 'answer'
        with patch.object(m, 'request_once', side_effect=response):
            m.complete('x', 'fake', 'm', [], 1, None, None, False, out)
        self.assertTrue(out.getvalue().endswith('\ranswer\n'))
        self.assertNotIn('\x1b', out.getvalue())

    def test_waiting_stream_stops_on_first_content(self):
        out = TTY()
        response = unittest.mock.MagicMock()
        def lines():
            self.assertIn('Đang chờ phản hồi', out.getvalue())
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
            self.assertEqual(len(runner.schemas), 4)
            self.assertEqual(json.loads(runner.execute('read_file', '{"path":"readme.txt"}'))['result'], 'hello\nworld')
            self.assertIn('2: world', runner.execute('search_text', '{"path":"readme.txt","query":"world"}'))
            for path in ('../outside', '/etc/passwd', '.env', 'ai.key', 'link'):
                self.assertIn('error', json.loads(runner.execute('read_file', json.dumps({'path': path}))))
            blocked = root / 'blocklist'
            blocked.write_text('# comment\nread_*\nsearch_text\n')
            runner = m.ToolRunner(str(root), ['list_files', 'run_command'], str(blocked))
            self.assertEqual(runner.schemas, [])
            self.assertIn('error', json.loads(runner.execute('read_file', '{"path":"readme.txt"}')))
            self.assertIn('error', json.loads(runner.execute('shell', '{}')))

    def test_linux_commands(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            result = json.loads(runner.execute('run_command', json.dumps({
                'command': 'printf hello > output.txt; cat output.txt; printf error >&2; exit 7'})))
            self.assertEqual(result['output'], 'helloerror')
            self.assertEqual(result['exit_code'], 7)
            self.assertEqual((Path(d) / 'output.txt').read_text(), 'hello')
            self.assertIsNone(result['stopped'])
            runner = m.ToolRunner(d, ['run_*'])
            self.assertIn('error', json.loads(runner.execute('run_command', '{"command":"touch blocked"}')))
            self.assertFalse((Path(d) / 'blocked').exists())

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
