#!/usr/bin/env python3
"""Security and failure-path tests; no real credentials or external network."""
import contextlib
import importlib.machinery
import importlib.util
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
def load_source(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


m = load_source("daniai_regression", AI)


class ChunkResponse:
    def __init__(self, chunks):
        self.chunks = iter(chunks)

    def read1(self, size):
        return next(self.chunks, b'')

    read = read1


class TTY(io.StringIO):
    def isatty(self):
        return True


class Tests(unittest.TestCase):
    def stream(self, data, out=None, live=None):
        out = out if out is not None else io.StringIO()
        with patch.object(m, "post_json", return_value=io.BytesIO(data)):
            return m.request_stream("http://local", "fake", "m", [], 1,
                                    None, None, out, live)

    def test_scan_cursor_all_pages_beyond_entry_and_match_limits(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for i in range(2050):
                (root / ('f%04d.txt' % i)).write_text('line')
            runner = m.ToolRunner(d)
            found = []
            cursor = None
            for _ in range(40):
                args = {'path': '.', 'pattern': '*.txt'}
                if cursor:
                    args['cursor'] = cursor
                page = json.loads(runner.execute('find_files', json.dumps(args)))
                self.assertNotIn('error', page)
                self.assertLessEqual(page['scanned'], 2000)
                self.assertLessEqual(len(page['result'].splitlines()), 100)
                found.extend(page['result'].splitlines())
                cursor = page['next_cursor']
                if cursor is None:
                    break
            self.assertIsNone(cursor)
            self.assertEqual(len(found), 2050)
            self.assertEqual(len(set(found)), 2050)
            self.assertFalse(runner.scan_cursors)

    def test_search_cursor_same_file_line_boundary_and_clipping(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'a.txt').write_text(''.join('match %d\n' % i for i in range(240)))
            (root / 'long.txt').write_text('match ' + 'x' * 20000)
            runner = m.ToolRunner(d)
            results = []
            cursor = None
            clipped = 0
            for _ in range(10):
                page = runner.scan_files('.', 'match', search=True, cursor=cursor)
                page = json.loads(page)
                self.assertLessEqual(len(page['result']), 16000)
                results.extend(page['result'].splitlines())
                clipped += page['clipped']
                cursor = page['next_cursor']
                if cursor is None:
                    break
            self.assertIsNone(cursor)
            self.assertEqual(len(results), 241)
            self.assertEqual(len(set(results)), 241)
            self.assertEqual(clipped, 1)
            self.assertTrue(any('[line clipped]' in r for r in results))

    def test_scan_cursor_expiry_reuse_policy_and_cleanup(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for i in range(105):
                (root / ('f%d.txt' % i)).write_text('x')
            runner = m.ToolRunner(d)
            def page(cursor=None):
                return json.loads(runner.scan_files('.', '*.txt', cursor=cursor))
            first = page()['next_cursor']
            self.assertIsNotNone(first)
            self.assertIsNone(page(first)['next_cursor'])
            with self.assertRaises(ValueError):
                page(first)
            cursor = page()['next_cursor']
            runner.deny_paths = ['f*']
            with self.assertRaises(ValueError):
                page(cursor)
            self.assertFalse(runner.scan_cursors)
            runner.deny_paths = []
            cursor = page()['next_cursor']
            with patch.object(m.time, 'monotonic', return_value=time.monotonic() + 121):
                with self.assertRaises(ValueError):
                    page(cursor)
            for _ in range(6):
                page()
            self.assertEqual(len(runner.scan_cursors), 4)
            runner.close_scans()
            self.assertFalse(runner.scan_cursors)
            with self.assertRaises(ValueError):
                runner.scan_files('.', '*', cursor=3)
            with self.assertRaises(ValueError):
                runner.scan_files('.', '*', cursor='unknown')

    def test_http_retry_headers_bounds_and_no_transport_retry(self):
        from unittest.mock import Mock
        import urllib.error
        token = m.HTTP_RETRIES.set(2)
        try:
            req = urllib.request.Request('https://example.test/v1/chat/completions', data=b'{}')
            opener = Mock()
            response = io.BytesIO(b'ok')
            opener.open.side_effect = [urllib.error.HTTPError(req.full_url, 429, 'busy', {'Retry-After': '0'}, io.BytesIO()), response]
            with patch.object(m.urllib.request, 'build_opener', return_value=opener), \
                 patch.object(m.time, 'sleep') as sleep, contextlib.redirect_stderr(io.StringIO()):
                self.assertIs(m.open_request(req, 10), response)
                self.assertEqual(opener.open.call_count, 2)
                sleep.assert_called_once_with(0.0)
            opener.open.reset_mock()
            opener.open.side_effect = urllib.error.URLError('ambiguous connection failure')
            with patch.object(m.urllib.request, 'build_opener', return_value=opener), self.assertRaises(m.AIError):
                m.open_request(req, 10)
            self.assertEqual(opener.open.call_count, 1)
            opener.open.reset_mock()
            opener.open.side_effect = urllib.error.HTTPError(req.full_url, 503, 'busy', {'Retry-After': '60'}, io.BytesIO())
            with patch.object(m.urllib.request, 'build_opener', return_value=opener), \
                 patch.object(m.time, 'sleep') as sleep, self.assertRaises(m.AIError):
                m.open_request(req, 1)
            sleep.assert_not_called()
            self.assertEqual(opener.open.call_count, 1)
            opener.open.reset_mock()
            opener.open.side_effect = lambda *a, **kw: (_ for _ in ()).throw(
                urllib.error.HTTPError(req.full_url, 502, 'busy', {'Retry-After': '0'}, io.BytesIO()))
            with patch.object(m.urllib.request, 'build_opener', return_value=opener), \
                 patch.object(m.time, 'sleep'), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(m.AIError):
                m.open_request(req, 10)
            self.assertEqual(opener.open.call_count, 3)
        finally:
            m.HTTP_RETRIES.reset(token)

    def test_retry_after_mutation_does_not_replay_tool(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            first = {'choices': [{'message': {'role': 'assistant', 'content': None, 'tool_calls': [
                {'id': 'c', 'type': 'function', 'function': {'name': 'write_file',
                 'arguments': '{"path":"once.txt","content":"once"}'}}]}}]}
            final = {'choices': [{'message': {'role': 'assistant', 'content': 'done'}}]}
            token = m.HTTP_RETRIES.set(1)
            try:
                with patch.object(m, 'open_request_once', side_effect=[
                        io.BytesIO(json.dumps(first).encode()), m.RetryableHTTPError(503, 0),
                        io.BytesIO(json.dumps(final).encode())]) as request, \
                     patch.object(runner, 'execute', wraps=runner.execute) as tool, \
                     patch.object(m.time, 'sleep'), contextlib.redirect_stderr(io.StringIO()):
                    result = m.complete('http://local', 'fake', 'm', [{'role': 'user', 'content': 'write'}],
                                        10, None, None, False, io.StringIO(), runner=runner)
                self.assertEqual(result, 'done')
                self.assertEqual(tool.call_count, 1)
                self.assertEqual(request.call_count, 3)
                self.assertEqual((Path(d) / 'once.txt').read_text(), 'once')
            finally:
                m.HTTP_RETRIES.reset(token)

    def test_turn_deadline_stops_next_tool_preserves_first_result(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            clock = [100.0]
            calls = [{'id': str(i), 'type': 'function', 'function': {
                'name': 'write_file', 'arguments': json.dumps({'path': str(i) + '.txt', 'content': 'done'})}}
                for i in range(2)]
            execute = runner.execute
            def slow_execute(*args):
                result = execute(*args)
                clock[0] += 2
                return result
            messages = [{'role': 'user', 'content': 'write'}]
            token = m.TURN_TIMEOUT.set(1)
            try:
                with patch.object(m.time, 'monotonic', side_effect=lambda: clock[0]), \
                     patch.object(m, 'request_once', return_value={'role': 'assistant', 'content': None, 'tool_calls': calls}), \
                     patch.object(runner, 'execute', side_effect=slow_execute) as tool:
                    with self.assertRaises(m.AIError):
                        m.complete('http://local', 'fake', 'm', messages, 60, None, None, False, io.StringIO(), runner=runner)
                self.assertEqual(tool.call_count, 1)
                self.assertTrue((Path(d) / '0.txt').exists())
                self.assertFalse((Path(d) / '1.txt').exists())
                self.assertIn('created', messages[2]['content'])
                self.assertIn('unknown', messages[3]['content'])
                self.assertIsNone(m.TURN_DEADLINE.get())
            finally:
                m.TURN_TIMEOUT.reset(token)

    def test_turn_deadline_body_subprocess_and_confirmation(self):
        token = m.TURN_DEADLINE.set(time.monotonic() - 1)
        try:
            with self.assertRaises(m.AIError):
                m.response_chunk(io.BytesIO(b'ok'), 2, time.monotonic() + 60)
            with patch.object(m.subprocess, 'Popen') as spawn, self.assertRaises(m.AIError):
                m._run_tool_process(['/bin/true'], '.', {}, b'', 30, 100)
            spawn.assert_not_called()
        finally:
            m.TURN_DEADLINE.reset(token)
        with tempfile.TemporaryDirectory() as d:
            clock = [100.0]
            def confirm(*args):
                clock[0] += 2
                return True
            runner = m.ToolRunner(d, permissions='ask', confirm=confirm)
            with patch.object(m.time, 'monotonic', side_effect=lambda: clock[0]):
                token = m.TURN_DEADLINE.set(101.0)
                try:
                    with self.assertRaises(m.AIError):
                        runner.execute('write_file', '{"path":"a.txt","content":"no"}')
                    self.assertFalse((Path(d) / 'a.txt').exists())
                finally:
                    m.TURN_DEADLINE.reset(token)
        for args in (['--turn-timeout', 'nan'], ['--turn-timeout', '0'], ['--http-retries', '4']):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                m.main(args)

    def test_file_turn_diff_undo_and_mode(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            path = root / 'a.txt'
            path.write_text('original\n')
            path.chmod(0o640)
            runner = m.ToolRunner(d)
            runner.begin_file_turn()
            for old, new in [('original', 'first'), ('first', 'second')]:
                result = json.loads(runner.execute('edit_file', json.dumps(
                    {'path': 'a.txt', 'old_text': old, 'new_text': new})))
                self.assertNotIn('error', result)
            self.assertNotIn('error', json.loads(runner.execute('write_file',
                             '{"path":"new.txt","content":"new file"}')))
            self.assertIn('-original', runner.file_diff())
            self.assertIn('+second', runner.file_diff())
            self.assertIn('/dev/null', runner.file_diff())
            self.assertEqual(set(runner.undo_files()), {'a.txt', 'new.txt'})
            self.assertEqual(path.read_text(), 'original\n')
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o640)
            self.assertFalse((root / 'new.txt').exists())
            self.assertFalse(runner.file_changes)

    def test_undo_refuses_user_changes_and_preflights_all(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            runner = m.ToolRunner(d)
            for name in ('a.txt', 'b.txt'):
                (root / name).write_text('before')
                runner.change_file('edit_file', {'path': name, 'old_text': 'before', 'new_text': 'after'})
            (root / 'b.txt').write_text('user changed')
            with self.assertRaises(ValueError):
                runner.undo_files()
            self.assertEqual((root / 'a.txt').read_text(), 'after')
            self.assertEqual((root / 'b.txt').read_text(), 'user changed')
            self.assertEqual(len(runner.file_changes), 2)
            runner.change_file('edit_file', {'path': 'b.txt', 'old_text': 'user changed', 'new_text': 'ai again'})
            with self.assertRaises(ValueError):
                runner.undo_files()
            runner.begin_file_turn()
            self.assertFalse(runner.file_changes)

    def test_undo_rejects_symlink_policy_and_budget(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            runner = m.ToolRunner(d)
            runner.change_file('write_file', {'path': 'a.txt', 'content': 'AI'})
            (root / 'a.txt').unlink()
            (root / 'user.txt').write_text('user')
            (root / 'a.txt').symlink_to('user.txt')
            with self.assertRaises(OSError):
                runner.undo_files()
            self.assertEqual((root / 'user.txt').read_text(), 'user')
            runner.begin_file_turn()
            runner.change_bytes = 8 * 1024 * 1024
            with self.assertRaises(ValueError):
                runner.change_file('write_file', {'path': 'budget.txt', 'content': 'no'})
            self.assertFalse((root / 'budget.txt').exists())
            runner.begin_file_turn()
            runner.change_file('write_file', {'path': 'allowed.txt', 'content': 'AI'})
            runner.deny_paths = ['allowed.txt']
            with self.assertRaises(ValueError):
                runner.undo_files()
            self.assertTrue((root / 'allowed.txt').exists())

    def test_repl_undo_local_notice_and_diff(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'a.txt'
            path.write_text('before')
            runner = m.ToolRunner(d, confirm=lambda *a: True)
            seen = []
            def complete(*args):
                messages = args[3]
                seen.append(messages)
                runner.change_file('edit_file', {'path': 'a.txt', 'old_text': 'before', 'new_text': 'after'})
                return 'done'
            with patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=['edit', '/diff', '/undo', '/q']), \
                 patch.object(m, 'complete', side_effect=complete) as request, \
                 contextlib.redirect_stdout(io.StringIO()) as out:
                m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d, runner=runner)
            self.assertEqual(request.call_count, 1)
            self.assertEqual(path.read_text(), 'before')
            self.assertIn('-before', out.getvalue())
            self.assertIn('restored 1 files', out.getvalue())
            self.assertIn('Previous tool results', seen[0][-1]['content'])

    def test_path_policy_read_scan_and_mutations(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'private').mkdir()
            (root / 'private/data.txt').write_text('hidden marker')
            (root / 'public').mkdir()
            (root / 'public/a.txt').write_text('public marker')
            runner = m.ToolRunner(d, deny_paths=['private', '*backup*'],
                                  write_paths=['public', 'public/*'], include_ignored=True,
                                  confirm=lambda *a: True, permissions='ask')
            def execute(name, **args):
                return json.loads(runner.execute(name, json.dumps(args)))
            for name in ('read_file', 'search_text', 'list_files', 'find_files', 'search_files'):
                args = {'path': 'private/data.txt' if name in ('read_file', 'search_text') else 'private'}
                if name in ('search_text', 'search_files'):
                    args['query'] = 'marker'
                if name == 'find_files':
                    args['pattern'] = '*'
                self.assertIn('error', execute(name, **args))
            for name, extra in [('list_files', {}), ('find_files', {'pattern': '*'}),
                                ('search_files', {'query': 'marker'})]:
                result = execute(name, path='.', **extra)
                self.assertNotIn('private', json.dumps(result))
                self.assertNotIn('hidden marker', json.dumps(result))
            for name, args in [('write_file', {'path': 'outside.txt', 'content': 'x'}),
                               ('edit_file', {'path': 'private/data.txt', 'old_text': 'hidden', 'new_text': 'x'}),
                               ('make_directory', {'path': 'outside'}),
                               ('write_file', {'path': 'public/backup.txt', 'content': 'x'})]:
                self.assertIn('error', execute(name, **args))
            self.assertNotIn('error', execute('write_file', path='public/new.txt', content='ok'))
            self.assertNotIn('error', execute('make_directory', path='public/sub'))
            self.assertEqual((root / 'private/data.txt').read_text(), 'hidden marker')
            self.assertFalse((root / 'outside.txt').exists())
            # Policy applies to direct builtin methods too.
            with self.assertRaises(ValueError):
                runner.open_path('public/../private/data.txt')

    def test_domain_policy_redirect_and_origin_approval(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory() as d:
            confirm = Mock(return_value=True)
            runner = m.ToolRunner(d, permissions='ask', confirm=confirm,
                                  allow_domains=['EXAMPLE.COM.', 'other.example'])
            runner.authorize_url('https://example.com/path')
            runner.authorize_url('https://EXAMPLE.COM/another')
            self.assertEqual(confirm.call_count, 1)
            runner.authorize_url('https://example.com:8443/path')
            self.assertEqual(confirm.call_count, 2)
            with self.assertRaises(m.AIError):
                runner.authorize_url('https://sub.example.com/')
            self.assertEqual(confirm.call_count, 2)
            confirm.return_value = False
            with self.assertRaises(m.AIError):
                runner.authorize_url('https://other.example/')
            self.assertNotIn(('https', 'other.example', 443), runner.approved_origins)
            # Every redirect is authorized before DNS/connection to its host.
            response = Mock(status=302)
            response.getheader.return_value = 'https://blocked.example/private'
            connection = Mock()
            connection.getresponse.return_value = response
            connection.sock.getpeername.return_value = ('93.184.216.34', 443)
            confirm.return_value = True
            with patch.object(m.socket, 'getaddrinfo', return_value=[(2, 1, 6, '', ('93.184.216.34', 443))]) as dns, \
                 patch.object(m.http.client, 'HTTPSConnection', return_value=connection):
                result = json.loads(runner.fetch_url('https://example.com/start'))
                self.assertIn('not in allowlist', result['error'])
                self.assertEqual(dns.call_count, 1)
            readonly = m.ToolRunner(d, permissions='read-only', confirm=lambda *a: True)
            self.assertIn('error', json.loads(readonly.execute('fetch_url', '{"url":"https://example.com"}')))

    def test_path_domain_policy_validation(self):
        with tempfile.TemporaryDirectory() as d:
            for pattern in ('', '/etc/*', '../private', 'a/../b', 'a\x00b'):
                with self.assertRaises(m.AIError):
                    m.ToolRunner(d, deny_paths=[pattern])
            for domain in ('https://example.com', '*.example.com', 'example.com:443', '', 'a/b'):
                with self.assertRaises(m.AIError):
                    m.ToolRunner(d, allow_domains=[domain])
            runner = m.ToolRunner(d, deny_paths=['private/'])
            self.assertTrue(runner.path_denied('private/nested/file'))
            self.assertFalse(runner.path_denied('other/private/file'))

    def test_usage_nonstream_and_stream(self):
        tracker = m.UsageTracker()
        token = m.ACTIVE_USAGE.set(tracker)
        option = m.STREAM_USAGE.set(True)
        try:
            response = {'choices': [{'message': {'content': 'ok'}}],
                        'usage': {'prompt_tokens': 10, 'completion_tokens': 2, 'total_tokens': 12}}
            with patch.object(m, 'post_json', return_value=io.BytesIO(json.dumps(response).encode())):
                self.assertEqual(m.request_once('http://local', 'fake', 'm', [], 1, None, None), 'ok')
            data = (b'data: {"choices":[{"delta":{"content":"ok"}}]}\n\n'
                    b'data: {"choices":[],"usage":{"prompt_tokens":20,"completion_tokens":3,"total_tokens":23}}\n\n'
                    b'data: [DONE]\n\n')
            with patch.object(m, 'post_json', return_value=io.BytesIO(data)) as post:
                m.request_stream('http://local', 'fake', 'm', [], 1, None, None, io.StringIO())
            self.assertEqual(post.call_args.args[1]['stream_options'], {'include_usage': True})
            self.assertEqual(tracker.totals, {'prompt_tokens': 30, 'completion_tokens': 5, 'total_tokens': 35})
            tracker.record({'usage': {'prompt_tokens': True, 'completion_tokens': -1, 'total_tokens': '100'}})
            tracker.record({})
            self.assertEqual(tracker.requests, 4)
            self.assertEqual(tracker.reports, 2)
            self.assertIsNone(tracker.last)
            self.assertNotIn('stream_options', m.chat_payload('m', [], False, None, None, None))
            with patch.object(m, 'post_json', return_value=io.BytesIO(data.split(b'data: [DONE]')[0])):
                with self.assertRaises(m.AIError):
                    m.request_stream('http://local', 'fake', 'm', [], 1, None, None, io.StringIO())
            self.assertEqual(tracker.requests, 4)  # incomplete stream is not counted as completed
        finally:
            m.STREAM_USAGE.reset(option)
            m.ACTIVE_USAGE.reset(token)

    def test_compact_keeps_whole_tool_turns(self):
        messages = [{'role': 'system', 'content': 'rules'}, {'role': 'user', 'content': 'old'},
                    {'role': 'assistant', 'content': 'old answer'}, {'role': 'user', 'content': 'recent'},
                    {'role': 'assistant', 'content': None, 'tool_calls': [
                        {'id': 'c', 'type': 'function', 'function': {'name': 'read_file', 'arguments': '{}'}}]},
                    {'role': 'tool', 'tool_call_id': 'c', 'content': 'result'},
                    {'role': 'assistant', 'content': 'new answer'}]
        compacted, dropped = m.compact_messages(messages, 1)
        self.assertEqual(dropped, 1)
        self.assertEqual(compacted, messages[:1] + messages[3:])
        self.assertEqual(m.compact_messages(messages, 4), (messages, 0))
        for bad in (0, -1, True):
            with self.assertRaises(m.AIError):
                m.compact_messages(messages, bad)
        with tempfile.TemporaryDirectory() as d:
            path = str(Path(d) / 's.json')
            m.save_session(path, 'm', compacted)
            self.assertEqual(m.load_session(path)[1], compacted)

    def test_compact_repl_branch_private_and_local_usage(self):
        original = [{'role': 'system', 'content': 'rules'}]
        for i in range(3):
            original.extend([{'role': 'user', 'content': str(i)}, {'role': 'assistant', 'content': 'answer'}])
        for private in (False, True):
            with tempfile.TemporaryDirectory() as d, \
                 patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=['/compact 0', '/compact 1', '/usage', '/q']), \
                 patch.object(m, 'complete') as complete, contextlib.redirect_stdout(io.StringIO()) as out:
                m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d,
                       initial=('m', original), no_history=private)
                complete.assert_not_called()
                self.assertIn('dropped 2 old turns', out.getvalue())
                self.assertIn('0 completed responses', out.getvalue())
                if private:
                    self.assertFalse(list(Path(d).iterdir()))
                else:
                    sessions = [json.loads(p.read_text()) for p in Path(d).glob('session-*.json')]
                    self.assertEqual(sorted(len(s['messages']) for s in sessions), [3, 7])
                    self.assertEqual(len(json.loads((Path(d) / 'last.json').read_text())['messages']), 3)

    def test_compact_checkpoint_failure_preserves_memory(self):
        original = [{'role': 'user', 'content': 'old'}, {'role': 'assistant', 'content': 'old answer'},
                    {'role': 'user', 'content': 'new'}, {'role': 'assistant', 'content': 'new answer'}]
        with tempfile.TemporaryDirectory() as d, \
             patch.object(m, 'setup_readline', return_value=''), \
             patch.object(m, 'input_boxed', side_effect=['/compact 1', '/context', '/q']), \
             patch.object(m.SessionStore, 'checkpoint', side_effect=[m.AIError('disk full'), None]), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d, initial=('m', original))
            self.assertIn('compact failed: disk full', out.getvalue())
            self.assertIn('4 messages', out.getvalue())

    def profile_fixture(self, directory, **caps):
        profile = {'base': 'https://profile.test/v1', 'key_env': 'TEST_PROFILE_KEY',
                   'model': 'profile-model', 'capabilities': dict(
                       tools=False, vision=False, stream=False, token_parameter='max_completion_tokens')}
        profile['capabilities'].update(caps)
        path = Path(directory) / 'profiles.json'
        path.write_text(json.dumps({'version': 1, 'profiles': {'test': profile}}))
        return path, profile

    def test_profile_key_isolation_and_payload(self):
        with tempfile.TemporaryDirectory() as d:
            path, profile = self.profile_fixture(d)
            env = {'DANI_AI_API_KEY': 'wrong-key', 'DANI_AI_API_BASE': 'https://wrong.test',
                   'DANI_AI_MODEL': 'wrong-model', 'TEST_PROFILE_KEY': 'right-key', 'XDG_CONFIG_HOME': d}
            with patch.dict(os.environ, env, clear=True), \
                 patch.object(m, 'read_piped_stdin', return_value=''), \
                 patch.object(m, 'post_json', return_value=io.BytesIO(
                     b'{"choices":[{"message":{"content":"ok"}}]}')) as post, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.main(['--profile', 'test', '--profiles-file', str(path),
                                         '--max-tokens', '123', 'hello']), 0)
            url, payload, key, _ = post.call_args.args
            self.assertEqual(url, profile['base'] + '/chat/completions')
            self.assertEqual(key, 'right-key')
            self.assertEqual(payload['model'], 'profile-model')
            self.assertFalse(payload['stream'])
            self.assertNotIn('tools', payload)
            self.assertEqual(payload['max_completion_tokens'], 123)
            self.assertNotIn('max_tokens', payload)
            self.assertIsNone(m.ACTIVE_PROFILE.get())
            with patch.dict(os.environ, {'DANI_AI_API_KEY': 'wrong-key', 'XDG_CONFIG_HOME': d}, clear=True), \
                 contextlib.redirect_stderr(io.StringIO()), patch.object(m, 'post_json') as post:
                self.assertEqual(m.main(['--profile', 'test', '--profiles-file', str(path), 'hello']), 2)
                post.assert_not_called()

    def test_profile_capability_guards(self):
        with tempfile.TemporaryDirectory() as d:
            path, _ = self.profile_fixture(d)
            profile = m.load_profile('test', str(path))
            token = m.ACTIVE_PROFILE.set(profile)
            try:
                images = [{'role': 'user', 'content': [{'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,YQ=='}}]}]
                cases = [('wrong-model', [], False, None, None),
                         ('profile-model', [], True, None, None),
                         ('profile-model', [], False, None, []),
                         ('profile-model', images, False, None, None)]
                for model, messages, stream, limit, tools in cases:
                    with self.assertRaises(m.AIError):
                        m.chat_payload(model, messages, stream, None, limit, tools)
                profile['capabilities']['token_parameter'] = None
                with self.assertRaises(m.AIError):
                    m.chat_payload('profile-model', [], False, None, 1, None)
                with self.assertRaises(m.AIError):
                    m.open_request(urllib.request.Request('https://other.test/v1/models'), 1)
            finally:
                m.ACTIVE_PROFILE.reset(token)

    def test_profile_stream_tools_and_session_context(self):
        with tempfile.TemporaryDirectory() as d:
            path, _ = self.profile_fixture(d, tools=True, vision=True, stream=True)
            profile = m.load_profile('test', str(path))
            original = m.session_context(profile['base'], d)
            token = m.ACTIVE_PROFILE.set(profile)
            try:
                context = m.session_context(profile['base'], d)
                self.assertEqual(context['profile']['name'], 'test')
                self.assertNotEqual(context, original)
                data = b'data: {"choices":[{"delta":{"content":"ok"}}]}\n\ndata: [DONE]\n\n'
                with patch.object(m, 'post_json', return_value=io.BytesIO(data)) as post:
                    reply = m.request_stream(profile['base'], 'fake', 'profile-model', [], 1,
                                             None, 42, io.StringIO(), tools=[])
                self.assertEqual(reply['content'], 'ok')
                payload = post.call_args.args[1]
                self.assertTrue(payload['stream'])
                self.assertEqual(payload['tools'], [])
                self.assertEqual(payload['max_completion_tokens'], 42)
                session = str(Path(d) / 's.json')
                m.save_session(session, 'profile-model', [], original)
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(m.load_session(session, context), (None, None))
            finally:
                m.ACTIVE_PROFILE.reset(token)

    def test_profile_validation_and_override_rejection(self):
        with tempfile.TemporaryDirectory() as d:
            path, profile = self.profile_fixture(d)
            for change in ({'base': 'https://user:password@host/v1'},
                           {'base': 'https://host/v1?key=secret'},
                           {'key_env': ''}, {'capabilities': {'tools': 'yes'}},
                           {'unexpected': 'secret'}):
                path.write_text(json.dumps({'version': 1, 'profiles': {'test': dict(profile, **change)}}))
                with self.assertRaises(m.AIError):
                    m.load_profile('test', str(path))
            path, _ = self.profile_fixture(d)
            for extra in (['--api-base', 'https://other.test'], ['--api-key', 'secret'], ['--model', 'other']):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    m.main(['--profile', 'test', '--profiles-file', str(path)] + extra + ['hello'])
            with self.assertRaises(m.AIError):
                m.load_profile('missing', str(path))

    def test_concurrent_session_checkpoints(self):
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            script = '''
import importlib.machinery, importlib.util, sys
loader = importlib.machinery.SourceFileLoader('ai', sys.argv[1])
spec = importlib.util.spec_from_loader('ai', loader)
m = importlib.util.module_from_spec(spec)
loader.exec_module(m)
s = m.SessionStore(sys.argv[2])
for i in range(12):
    s.checkpoint(sys.argv[3], [{'role':'user', 'content':str(i)}], None, 'complete')
'''
            children = [subprocess.Popen([sys.executable, '-c', script, str(AI), d, str(i)])
                        for i in range(4)]
            for child in children:
                self.assertEqual(child.wait(timeout=15), 0)
            sessions = list(Path(d).glob('session-*.json'))
            self.assertEqual(len(sessions), 4)
            objects = [json.loads(p.read_text()) for p in sessions]
            self.assertEqual({o['model'] for o in objects}, {'0', '1', '2', '3'})
            self.assertTrue(all(o['messages'][0]['content'] == '11' for o in objects))
            last = json.loads((Path(d) / 'last.json').read_text())
            match = next(o for o in objects if o['session_id'] == last['session_id'])
            self.assertGreaterEqual(last.pop('updated_at'), match.pop('updated_at'))
            self.assertEqual(last, match)
            for path in Path(d).iterdir():
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_readline_history_merge_preserves_other_repl(self):
        if m._rl is None:
            self.skipTest('readline unavailable')
        rl = m._rl
        with tempfile.TemporaryDirectory() as d:
            path = str(Path(d) / '.repl_history')
            try:
                rl.clear_history()
                rl.add_history('old')
                rl.write_history_file(path)
                # Both REPLs loaded one old entry; A finishes before B.
                rl.add_history('from A')
                m.merge_readline_history(path, 1)
                rl.clear_history()
                rl.add_history('old')
                rl.add_history('from B')
                m.merge_readline_history(path, 1)
                rl.clear_history()
                rl.read_history_file(path)
                self.assertEqual([rl.get_history_item(i) for i in range(1, 4)],
                                 ['old', 'from A', 'from B'])
                self.assertEqual(rl.get_current_history_length(), 3)
            finally:
                rl.clear_history()

    def test_session_branch_and_save_collision(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'original.json'
            messages = [{'role': 'user', 'content': 'original'}]
            m.save_session(str(source), 'm', messages, m.session_context('x', Path.cwd()))
            original = source.read_bytes()
            with patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=['/save original', 'hello', '/new', 'branch', '/q']), \
                 patch.object(m, 'complete', return_value='answer'), \
                 contextlib.redirect_stdout(io.StringIO()) as out:
                m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d,
                       initial=('m', messages))
            self.assertEqual(source.read_bytes(), original)
            self.assertIn('name already exists', out.getvalue())
            sessions = list(Path(d).glob('session-*.json'))
            self.assertEqual(len(sessions), 2)
            for path in sessions:
                self.assertEqual(json.loads(path.read_text())['session_id'], path.stem)

    def test_history_lock_released_and_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(RuntimeError):
                with m.history_lock(d):
                    raise RuntimeError('interrupted')
            with m.history_lock(d):
                pass
            lock = Path(d) / '.history.lock'
            lock.unlink()
            target = Path(d) / 'target'
            target.write_text('untouched')
            lock.symlink_to(target)
            with self.assertRaises(OSError):
                with m.history_lock(d):
                    self.fail('followed symlink')
            self.assertEqual(target.read_text(), 'untouched')

    def test_bounded_json_and_input(self):
        with self.assertRaises(m.AIError):
            m.bounded_response(io.BytesIO(b'x' * 33), time.monotonic() + 1, limit=32)
        with patch.object(m, 'MAX_STDIN_CHARS', 8), patch('sys.stdin', io.StringIO('x' * 9)):
            with self.assertRaises(m.AIError):
                m.read_piped_stdin()
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 's.json'
            path.write_text('x' * 33)
            with patch.object(m, 'MAX_SESSION_BYTES', 32), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(m.load_session(str(path)), (None, None))
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'saved.json'
            m.save_session(str(path), 'm', [])
            before = path.read_bytes()
            with patch.object(m, 'MAX_SESSION_BYTES', 8), contextlib.redirect_stderr(io.StringIO()):
                self.assertFalse(m.save_session(str(path), 'm', []))
            self.assertEqual(path.read_bytes(), before)
        with patch.object(m, 'MAX_SESSION_BYTES', 8), patch.object(m, 'open_request') as request:
            with self.assertRaises(m.AIError):
                m.post_json('x', {'long': 'payload'}, 'fake', 1)
            request.assert_not_called()

    def test_scan_ignores_and_override(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'node_modules').mkdir()
            (root / 'node_modules/a.py').write_text('needle')
            (root / 'generated').mkdir()
            (root / 'generated/a.py').write_text('needle')
            (root / 'main.py').write_text('needle')
            (root / '.aiignore').write_text('# explicit globs\ngenerated/\n')
            args = '{"path":".","pattern":"*.py"}'
            runner = m.ToolRunner(d)
            result = json.loads(runner.execute('find_files', args))
            self.assertEqual(result['result'], 'main.py')
            self.assertEqual(result['ignored'], 2)
            runner.include_ignored = True
            result = json.loads(runner.execute('find_files', args))
            self.assertIn('generated/a.py', result['result'])
            self.assertIn('node_modules/a.py', result['result'])
            (root / '.aiignore').unlink()
            (root / '.aiignore').symlink_to(root / 'main.py')
            runner.include_ignored = False
            self.assertIn('error', json.loads(runner.execute('find_files', args)))

    def test_session_export_delete_local_cli(self):
        with tempfile.TemporaryDirectory() as d:
            hist = Path(d) / 'history'
            hist.mkdir()
            messages = [{'role': 'system', 'content': 'private'},
                        {'role': 'user', 'content': 'hello ```\n# heading'}]
            m.save_session(str(hist / 'demo.json'), 'm', messages, {'root': '/tmp'})
            with patch.object(m, 'history_dir', return_value=str(hist)), \
                 patch.object(m, 'resolve_key', side_effect=AssertionError('must remain local')), \
                 contextlib.redirect_stdout(io.StringIO()):
                for fmt in ('json', 'md'):
                    target = Path(d) / ('export.' + fmt)
                    self.assertEqual(m.main(['history', 'export', 'demo', fmt, str(target)]), 0)
                    self.assertEqual(target.stat().st_mode & 0o777, 0o600)
                    self.assertIn('private', target.read_text())
                    if fmt == 'json':
                        self.assertEqual(m.load_session(str(target))[1], messages)
                    with self.assertRaises(m.AIError):
                        m.configured_main(['history', 'export', 'demo', fmt, str(target)])
                with self.assertRaises(m.AIError):
                    m.configured_main(['history', 'delete', 'demo'])
                self.assertTrue((hist / 'demo.json').exists())
                for command in (['history', 'delete', 'demo', 'CONFIRM'],
                                ['history', 'export', 'demo', 'json', str(Path(d) / 'blocked')]):
                    with self.assertRaises(m.AIError):
                        m.configured_main(['--no-history'] + command)
                self.assertEqual(m.main(['history', 'delete', 'demo', 'CONFIRM']), 0)
            self.assertFalse((hist / 'demo.json').exists())
            self.assertTrue((Path(d) / 'export.json').exists())

    def test_session_export_delete_file_guards(self):
        with tempfile.TemporaryDirectory() as d:
            obj = {'model': 'm', 'messages': []}
            target = Path(d) / 'target'
            target.write_text('keep')
            link = Path(d) / 'link.json'
            link.symlink_to(target)
            with self.assertRaises(OSError):
                m.export_session(obj, str(link), 'json')
            with self.assertRaises(OSError):
                m.delete_session(d, 'link')
            self.assertEqual(target.read_text(), 'keep')
            for name in ('../target', '/tmp/x', '', 'a.json'):
                with self.assertRaises(ValueError):
                    m.delete_session(d, name)
            (Path(d) / 'bad.json').write_text('{}')
            with self.assertRaises(ValueError):
                m.delete_session(d, 'bad')
            self.assertTrue((Path(d) / 'bad.json').exists())
            os.link(target, Path(d) / 'hard.json')
            with self.assertRaises(ValueError):
                m.delete_session(d, 'hard')
            self.assertFalse(any(p.name.startswith('.dani-ai-export-') for p in Path(d).iterdir()))

    def test_session_export_delete_repl(self):
        with tempfile.TemporaryDirectory() as d:
            target = Path(d) / 'current.json'
            m.save_session(str(Path(d) / 'saved.json'), 'm', [])
            out = io.StringIO()
            with patch.object(m, 'input_boxed', side_effect=[
                    '/export json ' + str(target), '/delete saved', '/delete saved', '/q']), \
                 patch('builtins.input', side_effect=['no', 'saved']), \
                 contextlib.redirect_stdout(out):
                m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d,
                       initial=('m', [{'role': 'user', 'content': 'hello'}]))
            self.assertEqual(m.load_session(str(target))[1][0]['content'], 'hello')
            self.assertFalse((Path(d) / 'saved.json').exists())
            self.assertIn('delete cancelled', out.getvalue())
            target.unlink()
            with patch.object(m, 'input_boxed', side_effect=[
                    '/export json ' + str(target), '/delete last', '/q']), \
                 contextlib.redirect_stdout(io.StringIO()):
                m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d, no_history=True)
            self.assertFalse(target.exists())

    def test_private_repl_never_persists(self):
        with tempfile.TemporaryDirectory() as d, \
             patch.object(m, 'setup_readline') as setup, \
             patch.object(m, 'save_session') as save, \
             patch.object(m, 'input_boxed', side_effect=['hello', '/save private', '/resume last', '/q']), \
             patch.object(m, 'complete', return_value='answer'), contextlib.redirect_stdout(io.StringIO()) as out:
            m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d, no_history=True)
            setup.assert_not_called()
            save.assert_not_called()
            self.assertFalse(list(Path(d).iterdir()))
            self.assertIn('save/resume/export/delete disabled', out.getvalue())
        # Restore normal history behavior for subsequent tests in this process.
        if m._rl is not None and hasattr(m._rl, 'set_auto_history'):
            m._rl.set_auto_history(True)

    def test_local_history_without_api_key(self):
        with tempfile.TemporaryDirectory() as d, patch.dict(os.environ, {'XDG_CONFIG_HOME': d}, clear=True), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            path = Path(d) / 'daniwm/ai-history/saved.json'
            m.save_session(str(path), 'm', [{'role': 'user', 'content': 'local hello'}])
            self.assertEqual(m.main(['history']), 0)
            self.assertIn('local hello', out.getvalue())

    def test_private_single_shot_tools_keep_no_checkpoint(self):
        with tempfile.TemporaryDirectory() as d, \
             patch.dict(os.environ, {'XDG_CONFIG_HOME': d, 'DANI_AI_API_KEY': 'fake'}, clear=True), \
             patch.object(m, 'read_piped_stdin', return_value=''), \
             patch.object(m, 'save_session') as save, \
             patch.object(m, 'request_once', side_effect=[
                 {'role': 'assistant', 'content': None, 'tool_calls': [
                     {'id': 'c', 'type': 'function', 'function': {'name': 'list_files', 'arguments': '{"path":"."}'}}]},
                 {'role': 'assistant', 'content': 'done'}]), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(m.main(['--no-history', '--no-stream', '--tool-root', d, 'list']), 0)
            save.assert_not_called()

    def test_context_command_is_local(self):
        with tempfile.TemporaryDirectory() as d, \
             patch.object(m, 'setup_readline', return_value=''), \
             patch.object(m, 'input_boxed', side_effect=['/context', '/q']), \
             patch.object(m, 'complete') as complete, contextlib.redirect_stdout(io.StringIO()) as out:
            m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d,
                   runner=m.ToolRunner(d))
            complete.assert_not_called()
            self.assertIn('0 messages', out.getvalue())
            self.assertIn('not token usage', out.getvalue())
            self.assertIn('64 tool calls', out.getvalue())

    def test_tool_turn_budgets(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d, max_tool_calls=1, max_tool_output=1)
            call = {'role': 'assistant', 'content': None, 'tool_calls': [
                {'id': 'c1', 'type': 'function', 'function': {'name': 'write_file',
                 'arguments': '{"path":"a","content":"written"}'}}]}
            messages = [{'role': 'user', 'content': 'create'}]
            with patch.object(m, 'request_once', return_value=call), self.assertRaises(m.AIError):
                m.complete('x', 'fake', 'm', messages, 1, None, None, False, io.StringIO(), runner=runner)
            self.assertTrue((Path(d) / 'a').exists())
            self.assertIn('DID execute', messages[-1]['content'])
            runner.max_tool_output = 100000
            call['tool_calls'].append(dict(call['tool_calls'][0], id='c2'))
            with patch.object(m, 'request_once', return_value=call), \
                 patch.object(runner, 'execute') as execute, self.assertRaises(m.AIError):
                m.complete('x', 'fake', 'm', [], 1, None, None, False, io.StringIO(), runner=runner)
            execute.assert_not_called()

    def test_nonstream_content_cap(self):
        data = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'long'}}]}).encode()
        for tools in (None, []):
            with patch.object(m, 'MAX_CONTENT_CHARS', 2), \
                 patch.object(m, 'post_json', return_value=io.BytesIO(data)), self.assertRaises(m.AIError):
                m.request_once('x', 'fake', 'm', [], 1, None, None, tools)

    def test_stream_budgets_and_fragmented_lines(self):
        data = b'data: {"choices":[{"delta":{"content":"hello"}}]}\ndata: [DONE]\n'
        response = ChunkResponse(bytes([c]) for c in data)
        lines = list(m.stream_lines(response, time.monotonic() + 1))
        self.assertEqual(b''.join(lines), data)
        for setting, limit, payload in [
                ('SSE_MAX_LINE', 8, b'data: ' + b'x' * 30),
                ('API_MAX_BYTES', 8, data),
                ('SSE_MAX_EVENTS', 2, b':ping\n' * 3 + data),
                ('MAX_CONTENT_CHARS', 2, data)]:
            with self.subTest(setting=setting), patch.object(m, setting, limit), self.assertRaises(m.AIError):
                self.stream(payload)

    def test_stream_tool_field_limits(self):
        for field in ('id', 'name'):
            call = {'index': 0, 'id': 'id', 'function': {'name': 'read_file', 'arguments': '{}'}}
            if field == 'id':
                call['id'] = 'x' * 257
            else:
                call['function']['name'] = 'x' * 129
            data = ('data: ' + json.dumps({'choices': [{'delta': {'tool_calls': [call]}}]}) +
                    '\ndata: [DONE]\n').encode()
            with patch.object(m, 'post_json', return_value=io.BytesIO(data)), self.assertRaises(m.AIError):
                m.request_stream('x', 'fake', 'm', [], 1, None, None, io.StringIO(), tools=[])

    def test_response_deadline_slow_drip_local_server(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                try:
                    for _ in range(100):
                        self.wfile.write(b'x')
                        self.wfile.flush()
                        time.sleep(0.02)
                except OSError:
                    pass
        server = HTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            start = time.monotonic()
            with urllib.request.urlopen('http://127.0.0.1:%d/' % server.server_port, timeout=1) as response:
                with self.assertRaises((m.AIError, OSError)):
                    m.bounded_response(response, start + 0.15)
            self.assertLess(time.monotonic() - start, 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

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
        response.__enter__.return_value = ChunkResponse(lines())
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
        response.__enter__.return_value = ChunkResponse(lines())
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
            m.save_session(str(path), 'm', old, m.session_context(m.DEFAULT_BASE, Path.cwd()))
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
        response.__enter__.return_value = ChunkResponse(lines())
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
            self.assertEqual(len(runner.schemas), 10)
            self.assertEqual(json.loads(runner.execute('read_file', '{"path":"readme.txt"}'))['result'], 'hello\nworld')
            self.assertIn('2: world', runner.execute('search_text', '{"path":"readme.txt","query":"world"}'))
            for path in ('../outside', '/etc/passwd', '.env', 'ai.key', 'link'):
                self.assertIn('error', json.loads(runner.execute('read_file', json.dumps({'path': path}))))
            blocked = root / 'blocklist'
            blocked.write_text('# comment\nread_*\nsearch_text\n')
            runner = m.ToolRunner(str(root), ['list_files', 'run_command', 'write_file',
                                              'edit_file', 'git_status', 'fetch_url', 'find_files',
                                              'search_files', 'make_directory'], str(blocked))
            self.assertEqual(runner.schemas, [])
            self.assertIn('error', json.loads(runner.execute('read_file', '{"path":"readme.txt"}')))
            self.assertIn('error', json.loads(runner.execute('shell', '{}')))

    def test_basic_tools_default_permissions_and_scan(self):
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as outside:
            root = Path(d)
            runner = m.ToolRunner(d, confirm=lambda *a: self.fail('basic tool prompted'))
            def execute(name, **args):
                return json.loads(runner.execute(name, json.dumps(args)))
            names = {s['function']['name'] for s in runner.schemas}
            self.assertEqual(names, set(m.TOOL_NAMES) - {'run_command'})
            self.assertIn('result', execute('make_directory', path='src/nested'))
            self.assertIn('result', execute('make_directory', path='src/nested'))
            self.assertEqual(stat.S_IMODE((root / 'src').stat().st_mode), 0o700)
            (root / 'src/a.py').write_text('hello\nneedle\n')
            (root / 'src/nested/b.py').write_text('needle\n')
            (root / '.env').write_text('needle')
            (root / 'secret.txt').write_text('needle')
            (root / 'binary').write_bytes(b'\x00needle')
            (root / 'big').write_text('x' * 65537)
            (root / 'link').symlink_to(outside, target_is_directory=True)
            os.link(root / 'src/a.py', root / 'hardlink')
            found = execute('find_files', path='.', pattern='*.py')
            # Hardlinked content is excluded, including the original name.
            self.assertEqual(found['result'], 'src/nested/b.py')
            self.assertFalse(found['truncated'])
            found = execute('search_files', path='.', query='needle')
            self.assertEqual(found['result'], 'src/nested/b.py:1: needle')
            self.assertEqual(found['skipped'], 2)
            for path in ('../escape', '/tmp/escape', '.env', 'secret-dir', 'link/new', 'src/a.py/new'):
                self.assertIn('error', execute('make_directory', path=path))
            self.assertFalse((Path(outside) / 'new').exists())
            for name, extra in (('find_files', {'pattern': '*'}), ('search_files', {'query': 'x'})):
                for path in ('../escape', 'link', '.env'):
                    self.assertIn('error', execute(name, path=path, **extra))
                self.assertIn('error', execute(name, path='.', **{k: '' for k in extra}))
            for name, args in (('make_directory', {'path': 'blocked'}),
                               ('find_files', {'path': '.', 'pattern': '*'}),
                               ('search_files', {'path': '.', 'query': 'needle'})):
                blocked = m.ToolRunner(d, [name])
                self.assertNotIn(name, [s['function']['name'] for s in blocked.schemas])
                self.assertIn('error', json.loads(blocked.execute(name, json.dumps(args))))
            (root / 'many.txt').write_text('needle\n' * 101)
            self.assertTrue(execute('search_files', path='.', query='needle')['truncated'])

    def test_read_file_pagination(self):
        with tempfile.TemporaryDirectory() as d:
            text = 'Tiếng Việt\n' * 8000
            (Path(d) / 'large.txt').write_text(text)
            runner = m.ToolRunner(d)
            def read(**args):
                return json.loads(runner.execute('read_file', json.dumps({'path': 'large.txt', **args})))
            page = read()
            self.assertTrue(page['truncated'])
            self.assertEqual(page['next_offset'], 16000)
            self.assertEqual(page['total_characters'], len(text))
            parts = [page['result']]
            while page['truncated']:
                page = read(offset=page['next_offset'])
                parts.append(page['result'])
            self.assertEqual(''.join(parts), text)
            self.assertIsNone(page['next_offset'])
            for args in ({'offset': -1}, {'limit': 0}, {'limit': 64001},
                         {'offset': True}, {'limit': '5'}, {'extra': 1}):
                self.assertIn('error', read(**args))
            self.assertEqual(read(offset=len(text) + 10)['result'], '')
            (Path(d) / 'large.txt').write_bytes(b'x' * (2 * 1024 * 1024 + 1))
            self.assertIn('error', read())

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

    def test_session_shell_toggle(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            self.assertTrue(runner.is_blocked('run_command'))
            out = io.StringIO()
            with patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=['/shell on', '/shell', '/shell off', '/q']), \
                 contextlib.redirect_stdout(out):
                m.repl('x', 'fake', 'm', '', 1, None, None, False, False, d, runner=runner)
            self.assertIn('UNSANDBOXED', out.getvalue())
            self.assertTrue(runner.is_blocked('run_command'))
            runner.set_shell(True)
            runner.set_shell(True)
            self.assertEqual(sum(s['function']['name'] == 'run_command' for s in runner.schemas), 1)
            self.assertEqual(json.loads(runner.execute('run_command', '{"command":"printf enabled"}'))['output'], 'enabled')
            blocked = m.ToolRunner(d, ['run_*'])
            blocked.set_shell(True)
            self.assertTrue(blocked.is_blocked('run_command'))
            self.assertNotIn('run_command', [s['function']['name'] for s in blocked.schemas])

    def test_repl_root_switch(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            hidden = root / '.config'
            hidden.mkdir()
            child = hidden / 'project'
            child.mkdir()
            runner = m.ToolRunner(d, ['write_file'])
            seen = []
            def complete(*args):
                seen.append((runner.root, list(args[3])))
                return 'ok'
            inputs = ['old question', '/root .config', '/root project',
                      '/root missing', '/root', 'new question', '/q']
            out = io.StringIO()
            with patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=inputs), \
                 patch.object(m, 'complete', side_effect=complete), \
                 contextlib.redirect_stdout(out):
                m.repl('x', 'fake', 'model', 'system', 1, None, None,
                       False, False, str(root / 'history'), runner=runner)
            self.assertEqual(runner.root, child)
            self.assertEqual(seen[0][0], root)
            self.assertEqual(seen[1][0], child)
            self.assertEqual(seen[1][1], [{'role': 'system', 'content': 'system'},
                                         {'role': 'user', 'content': 'new question'}])
            self.assertTrue(runner.is_blocked('write_file'))
            self.assertTrue(runner.is_blocked('run_command'))
            self.assertIn('cannot change root', out.getvalue())
            self.assertIn(str(child), out.getvalue())

    def test_fetch_url_extracts_text_and_blocks_private_hosts(self):
        seen, bodies = [], {
            '/page': ('text/html; charset=utf-8',
                      '<!doctype html><title>T &amp; x</title><body><h1>Hello</h1>'
                      '<script>bad()</script><a href="/rel">rel</a>'),
            '/json': ('application/json', '{"a": 1}'),
            '/bin': ('image/png', '\x89PNG'),
        }

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append(self.path)
                if self.path in bodies:
                    ctype, text = bodies[self.path]
                    body = text.encode()
                    self.send_response(200)
                    self.send_header('Content-Type', ctype)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                elif self.path in ('/redir', '/loop'):
                    self.send_response(302)
                    self.send_header('Location', '/page' if self.path == '/redir' else '/loop')
                    self.end_headers()
                else:
                    self.send_response(404)
                    self.end_headers()

            def log_message(self, *args):
                pass

        server = HTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as d:
                runner = m.ToolRunner(d)
                base = 'http://127.0.0.1:%d' % server.server_port

                def fetch(url):
                    return json.loads(runner.execute('fetch_url', json.dumps({'url': url})))

                self.assertNotIn('fetch_url', [s['function']['name'] for s in
                                               m.ToolRunner(d, ['fetch_*']).schemas])
                # Loopback and private targets are refused before any request.
                self.assertIn('refusing private', fetch(base + '/page')['error'])
                self.assertEqual(seen, [])
                with patch.object(m, 'web_check_address', lambda address: None):
                    page = fetch(base + '/page')
                    self.assertEqual(page['title'], 'T & x')
                    self.assertIn('Hello', page['text'])
                    self.assertNotIn('bad()', page['text'])
                    self.assertEqual(page['links'], [base + '/rel'])
                    self.assertTrue(page['untrusted'])
                    self.assertEqual(fetch(base + '/json')['text'], '{"a": 1}')
                    self.assertEqual(fetch(base + '/redir')['url'], base + '/page')
                    self.assertIn('redirects', fetch(base + '/loop')['error'])
                    self.assertIn('unsupported content type', fetch(base + '/bin')['error'])
                for bad in ('file:///etc/passwd', 'ftp://x.test/', 'http://u:p@x.test/',
                            'not a url', ''):
                    self.assertIn('error', fetch(bad))
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_fetch_url_bounds_and_decoding(self):
        self.assertEqual(m.web_charset('text/plain; charset=latin-1', b''), 'latin-1')
        self.assertEqual(m.web_charset('text/plain; charset=bogus', b''), 'utf-8')
        self.assertEqual(m.web_charset('text/html', b'<meta charset="windows-1252">'),
                         'windows-1252')
        self.assertFalse(m.web_is_text('image/png'))
        self.assertTrue(m.web_is_text(''))
        self.assertTrue(m.web_is_text('text/plain; charset=utf-8'))
        title, text = m.html_to_text('<title>T</title><style>x</style><p>a<b>b</b></p>')
        self.assertEqual(title, 'T')
        self.assertNotIn('x', text)
        self.assertEqual(text, 'ab')
        for address in ('127.0.0.1', '10.0.0.1', '192.168.1.1', '::1', '169.254.1.1',
                        '0.0.0.0', 'fd00::1', '100.64.0.1'):
            with self.assertRaises(m.AIError):
                m.web_check_address(address)
        m.web_check_address('93.184.216.34')
        with patch.object(m.socket, 'getaddrinfo', return_value=[
                (m.socket.AF_INET, m.socket.SOCK_STREAM, 6, '', ('127.0.0.1', 80))]):
            with self.assertRaises(m.AIError):
                m.web_get('http://example.test/')

    def test_git_status_without_shell(self):
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(['git', 'init', '-q', d], check=True)
            (Path(d) / 'new.txt').write_text('new')
            runner = m.ToolRunner(d)
            with patch.dict(os.environ, {'GIT_DIR': '/nonexistent',
                                         'GIT_WORK_TREE': '/nonexistent'}):
                result = json.loads(runner.execute('git_status', '{}'))
            self.assertNotIn('error', result)
            self.assertIn('?? new.txt', result['output'])
            self.assertEqual(result['tool_root'], d)
            self.assertIn('error', json.loads(runner.execute('git_status', '{"command":"rm -rf ."}')))
            blocked = m.ToolRunner(d, ['git_status'])
            self.assertIn('error', json.loads(blocked.execute('git_status', '{}')))
            self.assertIn('error', json.loads(runner.execute('run_command', '{"command":"true"}')))
        with tempfile.TemporaryDirectory() as d:
            self.assertIn('error', json.loads(m.ToolRunner(d).execute('git_status', '{}')))

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
        gold = load_source('gold_regression', AI.parent / 'ai.tools/xauusd_market.py')
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

    def test_btc_finite_data_and_candle_state(self):
        btc = load_source('btc_hardened', AI.parent / 'ai.tools/btc_market.py')
        for value in (True, False, 'NaN', 'Infinity', float('-inf'), None):
            self.assertIsNone(btc.number(value))
        rows = [[1000, '10', '12', '9', '11', '5', 2000, '55'],
                [3000, 'NaN', '12', '9', '11', '5', 4000]]
        out = io.StringIO()
        with patch.object(btc, 'get', return_value=rows), patch.object(btc.time, 'time', return_value=3), \
             patch('sys.stdin', io.StringIO('{"include":["klines"]}')), contextlib.redirect_stdout(out):
            self.assertEqual(btc.main(), 0)
        result = json.loads(out.getvalue())
        self.assertFalse(result['partial'])
        self.assertEqual(result['klines']['count'], 1)
        self.assertTrue(result['klines']['candles'][0]['is_closed'])
        self.assertEqual(result['timestamp_unit'], 'unix milliseconds')

    def test_btc_shape_errors_and_partial_budget(self):
        btc = load_source('btc_partial', AI.parent / 'ai.tools/btc_market.py')
        for args in ({'interval': []}, {'include': []}, {'extra': 1}):
            with patch('sys.stdin', io.StringIO(json.dumps(args))), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(btc.main(), 2)
        out = io.StringIO()
        with patch.object(btc, 'get', side_effect=[{'lastPrice': '100'}, TimeoutError('budget exhausted')]), \
             patch('sys.stdin', io.StringIO('{"include":["ticker","funding"]}')), contextlib.redirect_stdout(out):
            self.assertEqual(btc.main(), 0)
        result = json.loads(out.getvalue())
        self.assertEqual(result['ticker']['last'], 100)
        self.assertTrue(result['partial'])
        self.assertEqual(result['errors'][0]['section'], 'funding')
        for section, payload in [('ticker', []), ('klines', {}), ('long_short', [42])]:
            out = io.StringIO()
            with patch.object(btc, 'get', return_value=payload), \
                 patch('sys.stdin', io.StringIO(json.dumps({'include': [section]}))), contextlib.redirect_stdout(out):
                btc.main()
            self.assertTrue(json.loads(out.getvalue())['errors'])
        with patch.object(btc, 'TOTAL_TIMEOUT', 0), patch.object(btc, 'get') as get, \
             patch('sys.stdin', io.StringIO('{}')), contextlib.redirect_stdout(io.StringIO()):
            btc.main()
            get.assert_not_called()

    def test_btc_multiframe_partial_and_closed_windows(self):
        btc = load_source('btc_multiframe', AI.parent / 'ai.tools/btc_market.py')
        rows = [[1000, '10', '12', '9', '10', '5', 2000],
                [2001, '10', '22', '9', '20', '5', 9000]]
        args = {'include': ['klines'], 'interval': '1h', 'intervals': ['4h', '1h', '15m']}
        out = io.StringIO()
        with patch.object(btc, 'get', side_effect=[TimeoutError('budget'), rows, rows]) as get, \
             patch.object(btc.time, 'time', return_value=3), \
             patch('sys.stdin', io.StringIO(json.dumps(args))), contextlib.redirect_stdout(out):
            self.assertEqual(btc.main(), 0)
        result = json.loads(out.getvalue())
        self.assertTrue(result['partial'])
        self.assertEqual(result['errors'][0]['interval'], '4h')
        self.assertEqual(set(result['timeframes']), {'1h', '15m'})
        self.assertEqual(result['window']['change_pct'], 100)
        self.assertEqual(result['klines']['closed_window']['change_pct'], 0)
        self.assertEqual(result['klines'], result['timeframes']['1h'])
        self.assertEqual(len({c.kwargs['deadline'] for c in get.call_args_list}), 1)
        for bad in ([], ['1h', '1h'], ['4h'], ['1h', []], '1h',
                    ['1h', '4h', '1d', '1w', '15m']):
            with patch.object(btc, 'get') as get, \
                 patch('sys.stdin', io.StringIO(json.dumps({'intervals': bad}))), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(btc.main(), 2)
                get.assert_not_called()
        with patch.object(btc, 'get') as get, \
             patch('sys.stdin', io.StringIO('{"intervals":["1h","4h","1d"],"limit":200}')), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(btc.main(), 2)
            get.assert_not_called()

    def test_btc_oi_history_validation_and_changes(self):
        btc = load_source('btc_oi_history', AI.parent / 'ai.tools/btc_market.py')
        def run(rows, interval='1h'):
            out = io.StringIO()
            with patch.object(btc, 'get', return_value=rows) as get, \
                 patch.object(btc.time, 'time', return_value=5), \
                 patch('sys.stdin', io.StringIO(json.dumps({
                     'include': ['open_interest_history'], 'interval': interval}))), \
                 contextlib.redirect_stdout(out):
                self.assertEqual(btc.main(), 0)
            self.assertEqual(get.call_args.args[0], '/futures/data/openInterestHist')
            self.assertEqual(get.call_args.args[1]['period'], btc.LONG_SHORT_PERIODS.get(interval, interval))
            return json.loads(out.getvalue())
        rows = [{'timestamp': 1000, 'sumOpenInterest': '100', 'sumOpenInterestValue': '1000'},
                {'timestamp': 3000, 'sumOpenInterest': '110', 'sumOpenInterestValue': '1210'}]
        for interval in sorted(btc.INTERVALS):
            result = run(rows, interval)['open_interest_history']
            self.assertEqual(result['contracts_change_pct'], 10)
            self.assertEqual(result['value_change_pct'], 21)
            self.assertEqual(result['age_seconds'], 2)
        for empty in ([], rows[:1]):
            self.assertIsNone(run(empty)['open_interest_history']['contracts_change_pct'])
        for field in ('sumOpenInterest', 'sumOpenInterestValue', 'timestamp'):
            for value in (None, True, 'NaN', 'Infinity', '-1'):
                bad = dict(rows[0], **{field: value})
                self.assertTrue(run([bad])['partial'])
        for bad in ({}, [42], rows[::-1], [rows[0], rows[0]]):
            self.assertTrue(run(bad)['partial'])
        self.assertIsNone(btc.change_pct(0, 10))
        self.assertIsNone(btc.change_pct(1e-308, 1e308))

    def test_btc_multiframe_manifest_output_budget(self):
        btc = load_source('btc_output_budget', AI.parent / 'ai.tools/btc_market.py')
        manifest = json.loads((AI.parent / 'ai.tools/btc_market.json').read_text())
        rows = [[i * 1000, '123456.12345678', '123457.12345678', '123455.12345678',
                 '123456.12345678', '123456.12345678', i * 1000 + 999, '123456789.12345678']
                for i in range(200)]
        out = io.StringIO()
        with patch.object(btc, 'get', return_value=rows), \
             patch('sys.stdin', io.StringIO('{"include":["klines"],"intervals":["1h","4h"],"limit":200}')), \
             contextlib.redirect_stdout(out):
            self.assertEqual(btc.main(), 0)
        self.assertLess(len(out.getvalue().encode()), manifest['output_limit'])
        self.assertEqual(json.loads(out.getvalue())['klines']['count'], 200)
        self.assertNotIn('open_interest_history', btc.DEFAULT_SECTIONS)

    def test_btc_response_size_limit(self):
        btc = load_source('btc_limit', AI.parent / 'ai.tools/btc_market.py')
        with patch.object(btc, 'MAX_RESPONSE', 8), \
             patch.object(btc.urllib.request, 'urlopen', return_value=io.BytesIO(b'x' * 9)):
            with self.assertRaises(ValueError):
                btc.get('/test', {})

    def test_btc_ratio_units_and_periods(self):
        btc = load_source('btc_audit_regression', AI.parent / 'ai.tools/btc_market.py')
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
            self.assertEqual(len(missing.schemas), 10)

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
            self.assertEqual(messages[:len(before)], before)
            self.assertEqual(messages[-1]['role'], 'tool')

    def test_session_context_gate(self):
        with tempfile.TemporaryDirectory() as d:
            path = str(Path(d) / 's.json')
            context = m.session_context('https://provider.test/v1', d)
            m.save_session(path, 'm', [], context, 'interrupted')
            self.assertEqual(m.load_session(path, context), ('m', []))
            other = m.session_context('https://other.test/v1', d)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(m.load_session(path, other), (None, None))
                self.assertEqual(m.load_session(path, dict(context, root='/other')), (None, None))
                m.save_session(path, 'm', [])
                self.assertEqual(m.load_session(path, context), (None, None))
            self.assertEqual(m.load_session(path, context, True), ('m', []))

    def test_permission_presets_and_diff(self):
        with tempfile.TemporaryDirectory() as d:
            prompts = []
            runner = m.ToolRunner(d, permissions='ask', confirm=lambda n, text: prompts.append(text) or False)
            args = json.dumps({'path': 'a', 'content': 'hello\n'})
            self.assertIn('error', json.loads(runner.execute('write_file', args)))
            self.assertFalse((Path(d) / 'a').exists())
            self.assertIn('+hello', prompts[-1])
            runner.confirm = lambda n, text: True
            self.assertIn('result', json.loads(runner.execute('write_file', args)))
            read_only = m.ToolRunner(d, permissions='read-only', allow_shell=True,
                                     confirm=lambda *a: self.fail('read-only prompted'))
            for name, arguments in [('write_file', args), ('run_command', '{"command":"true"}'),
                                    ('fetch_url', '{"url":"https://example.com"}')]:
                self.assertTrue(read_only.is_blocked(name))
                self.assertIn('error', json.loads(read_only.execute(name, arguments)))
            self.assertIn('hello', read_only.execute('read_file', '{"path":"a"}'))

    def test_journal_retains_mutation_on_failure_and_cancel(self):
        for failure in (m.AIError('offline'), KeyboardInterrupt()):
            with tempfile.TemporaryDirectory() as d:
                runner = m.ToolRunner(d)
                path = str(Path(d) / 'checkpoint.json')
                runner.journal = lambda msgs, state: m.save_session(path, 'm', msgs, turn_state=state)
                messages = [{'role': 'user', 'content': 'create a'}]
                call = {'role': 'assistant', 'content': None, 'tool_calls': [
                    {'id': 'c1', 'type': 'function', 'function': {'name': 'write_file',
                     'arguments': json.dumps({'path': 'a', 'content': 'written'})}}]}
                with patch.object(m, 'request_once', side_effect=[call, failure]):
                    with self.assertRaises(type(failure)):
                        m.complete('x', 'fake', 'm', messages, 1, None, None, False,
                                   io.StringIO(), runner=runner)
                self.assertEqual((Path(d) / 'a').read_text(), 'written')
                self.assertEqual(m.load_session(path)[1], messages)
                self.assertEqual(messages[-1]['role'], 'tool')

    def test_repl_failed_tool_turn_is_saved(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            call = {'role': 'assistant', 'content': None, 'tool_calls': [
                {'id': 'c1', 'type': 'function', 'function': {'name': 'write_file',
                 'arguments': '{"path":"a","content":"written"}'}}]}
            with patch.object(m, 'setup_readline', return_value=''), \
                 patch.object(m, 'input_boxed', side_effect=['create', '/q']), \
                 patch.object(m, 'request_once', side_effect=[call, m.AIError('offline')]), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
                m.repl('https://provider.test/v1', 'fake', 'm', '', 1, None, None,
                       False, False, d, runner=runner)
            saved = json.loads((Path(d) / 'last.json').read_text())
            self.assertEqual(saved['turn_state'], 'interrupted')
            self.assertEqual(saved['context']['root'], d)
            self.assertEqual(saved['messages'][-1]['role'], 'tool')
            self.assertIn('tools may have changed files', err.getvalue())
            self.assertEqual(m.load_session(str(Path(d) / 'last.json'))[1], saved['messages'])

    def test_checkpoint_precedes_tool_and_survives_cancel(self):
        with tempfile.TemporaryDirectory() as d:
            runner = m.ToolRunner(d)
            messages = [{'role': 'user', 'content': 'do work'}]
            call = {'role': 'assistant', 'content': None, 'tool_calls': [
                {'id': 'c1', 'type': 'function', 'function': {'name': 'make_directory',
                 'arguments': '{"path":"new"}'}}]}
            def reject_checkpoint(*args):
                raise m.AIError('disk full')
            runner.journal = reject_checkpoint
            with patch.object(m, 'request_once', return_value=call), \
                 patch.object(runner, 'execute') as execute, self.assertRaises(m.AIError):
                m.complete('x', 'fake', 'm', messages, 1, None, None, False,
                           io.StringIO(), runner=runner)
            execute.assert_not_called()
            path = str(Path(d) / 'pending.json')
            runner.journal = lambda msgs, state: m.save_session(path, 'm', msgs, turn_state=state)
            messages = [{'role': 'user', 'content': 'do work'}]
            with patch.object(m, 'request_once', return_value=call), \
                 patch.object(runner, 'execute', side_effect=KeyboardInterrupt()), self.assertRaises(KeyboardInterrupt):
                m.complete('x', 'fake', 'm', messages, 1, None, None, False,
                           io.StringIO(), runner=runner)
            self.assertIn('execution status unknown', m.load_session(path)[1][-1]['content'])

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
                m.save_session(os.path.join(d, name + '.json'), 'saved-model', saved,
                               m.session_context('x', Path.cwd()))
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
