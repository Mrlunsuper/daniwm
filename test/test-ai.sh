#!/bin/sh
# test-ai.sh - headless tests for dani-ai (no X needed, mock HTTP server).
# Usage: ./test/test-ai.sh
set -u
TDIR=$(dirname "$0")
ROOT=$(cd "$TDIR/.." && pwd)
AI="$ROOT/dani-ai"
TMP=$(mktemp -d)
MOCK_LAST="$TMP/last-body.json"
export MOCK_LAST
pass=0
fail=0

ok() { pass=$((pass+1)); echo "PASS: $1"; }
bad() { fail=$((fail+1)); echo "FAIL: $1 (got: $2)"; }

# isolate config + keys from the real environment
EMPTY_CFG="$TMP/cfg"
mkdir -p "$EMPTY_CFG/daniwm"
clear_keys() {
    unset DANI_AI_API_KEY OPENAI_API_KEY DEEPSEEK_API_KEY FREELLMAPI_API_KEY VILAO_API_KEY
    unset DANI_AI_API_BASE OPENAI_API_BASE DANI_AI_MODEL DANI_AI_SYSTEM_FILE DANI_AI_KEY_FILE
    export XDG_CONFIG_HOME="$EMPTY_CFG"
}

cat > "$TMP/mock.py" <<'PYEOF'
import json, os
from http.server import BaseHTTPRequestHandler, HTTPServer

LAST = os.environ["MOCK_LAST"]

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.endswith("/models"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(
                {"data": [{"id": "m-b"}, {"id": "m-a"}]}).encode())
        else:
            self.send_response(404)
            self.end_headers()
    def do_POST(self):
        ln = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(ln).decode("utf-8")
        with open(LAST, "w") as f:
            f.write(body)
        try:
            obj = json.loads(body)
        except ValueError:
            obj = {}
        md = "MDTEST" in body
        if obj.get("stream") and "CALLTOOL" in body and "CUSTOM-OK" not in body:
            # First round: ask the model to call the custom tool.
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            call = {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "c1",
                    "type": "function", "function": {"name": "clock_probe",
                    "arguments": "{\"tag\":\"t1\"}"}}]}}]}
            self.wfile.write(("data: %s\n\n" % json.dumps(call)).encode())
            self.wfile.write(b"data: [DONE]\n\n")
            return
        if obj.get("stream") and "CUSTOM-OK" in body:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(("data: %s\n\n" % json.dumps(
                {"choices": [{"delta": {"content": "tool-answer"}}]})).encode())
            self.wfile.write(b"data: [DONE]\n\n")
            return
        if obj.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            toks = ("**bold**", " and ", "`code`") if md else ("mock-", "reply")
            for tok in toks:
                chunk = json.dumps({"choices": [{"delta": {"content": tok}}]})
                self.wfile.write(("data: %s\n\n" % chunk).encode())
            self.wfile.write(b"data: [DONE]\n\n")
        else:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            text = "**bold** and `code`" if md else "mock-reply"
            self.wfile.write(json.dumps(
                {"choices": [{"message": {"content": text}}]}).encode())
    def log_message(self, *a):
        pass

server = HTTPServer(("127.0.0.1", 0), H)
with open(os.path.join(os.path.dirname(LAST), "port"), "w") as f:
    f.write(str(server.server_port))
server.serve_forever()
PYEOF

python3 "$TMP/mock.py" &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT INT TERM
for _ in $(seq 1 50); do
    if [ -s "$TMP/port" ]; then
        break
    fi
    sleep 0.1
done
if [ ! -s "$TMP/port" ]; then
    echo "FAIL: mock server did not start" >&2
    exit 1
fi
PORT=$(cat "$TMP/port")
BASE="http://127.0.0.1:$PORT/v1"

# 1. missing key -> exit 2
clear_keys
out=$("$AI" --api-base "$BASE" --no-stream "hi" 2>&1); rc=$?
if [ $rc -eq 2 ]; then ok "missing key exits 2"; else bad "missing key exits 2" "rc=$rc out=$out"; fi

# 2. key priority DANI > OPENAI
clear_keys
out=$(DANI_AI_API_KEY=AAA OPENAI_API_KEY=BBB "$AI" --show-config | grep '^key')
case "$out" in
    *DANI_AI_API_KEY*) ok "key priority DANI first" ;;
    *) bad "key priority DANI first" "$out" ;;
esac

# 3. deepseek auto base/model
clear_keys
out=$(DEEPSEEK_API_KEY=x "$AI" --show-config)
echo "$out" | grep -q "api.deepseek.com" && echo "$out" | grep -q "deepseek-chat" \
    && ok "deepseek auto base+model" || bad "deepseek auto base+model" "$out"

# 4. non-stream single-shot + system forwarded
clear_keys
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream --system "SYS123" "hello" 2>&1); rc=$?
if [ "$out" = "mock-reply" ] && [ $rc -eq 0 ]; then ok "single-shot non-stream"; else bad "single-shot non-stream" "rc=$rc out=$out"; fi
if grep -q "SYS123" "$MOCK_LAST" && grep -q "hello" "$MOCK_LAST"; then ok "system+prompt forwarded"; else bad "system+prompt forwarded" "$(cat "$MOCK_LAST")"; fi

# 5. stream single-shot
clear_keys
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" "hello" 2>&1); rc=$?
if [ "$out" = "mock-reply" ] && [ $rc -eq 0 ]; then ok "single-shot stream"; else bad "single-shot stream" "rc=$rc out=$out"; fi

# 6. REPL with /reset
clear_keys
out=$(printf 'hello\n/reset\nquit\n' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream chat 2>&1)
echo "$out" | grep -q "mock-reply" && echo "$out" | grep -q "history cleared" \
    && ok "repl + /reset" || bad "repl + /reset" "$out"

# 7. pipe stdin merges with arg prompt
clear_keys
out=$(printf 'pipe-body' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream "summarize" 2>&1); rc=$?
if [ "$out" = "mock-reply" ] && grep -q "pipe-body" "$MOCK_LAST" && grep -q "summarize" "$MOCK_LAST"; then
    ok "pipe stdin merged"
else
    bad "pipe stdin merged" "rc=$rc out=$out body=$(cat "$MOCK_LAST")"
fi

# 8. missing --system-file -> exit 1
clear_keys
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --system-file /nope/missing.txt --show-config 2>&1); rc=$?
if [ $rc -eq 1 ]; then ok "missing system-file exits 1"; else bad "missing system-file exits 1" "rc=$rc"; fi

# 9. default ai.system file picked up
clear_keys
echo "FILE_SYS_456" > "$EMPTY_CFG/daniwm/ai.system"
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --show-config | grep '^system')
case "$out" in
    *ai.system*) ok "default ai.system picked up" ;;
    *) bad "default ai.system picked up" "$out" ;;
esac

# 10. ai.key file fallback (key + base + model), no env
clear_keys
printf 'filekey123\n%s\nfilemodel-x\n' "$BASE" > "$EMPTY_CFG/daniwm/ai.key"
out=$("$AI" --show-config)
echo "$out" | grep -q "key = .*ai.key" && echo "$out" | grep -q "model = filemodel-x" \
    && ok "ai.key fallback picked up" || bad "ai.key fallback picked up" "$out"
out=$("$AI" --no-stream "hello" 2>&1); rc=$?
if [ "$out" = "mock-reply" ] && [ $rc -eq 0 ]; then ok "ai.key single-shot works"; else bad "ai.key single-shot works" "rc=$rc out=$out"; fi
if grep -q "filemodel-x" "$MOCK_LAST"; then ok "ai.key model sent"; else bad "ai.key model sent" "$(cat "$MOCK_LAST")"; fi

# 11. piped output keeps raw markdown (no ANSI)
clear_keys
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream "MDTEST please" 2>&1)
case "$out" in
    "**bold** and \`code\`") ok "pipe keeps raw markdown" ;;
    *) bad "pipe keeps raw markdown" "$out" ;;
esac
if printf '%s' "$out" | grep -q "$(printf '\033')"; then
    bad "pipe has no ANSI" "found ESC"
else
    ok "pipe has no ANSI"
fi

# 12. renderer unit tests (import functions directly)
clear_keys
if python3 - "$AI" <<'PYEOF'; then
import sys
from importlib.machinery import SourceFileLoader
m = SourceFileLoader("daniai", sys.argv[1]).load_module()
r = m.render_markdown
B = "\x1b[1m"
fails = []
def check(name, cond):
    if not cond:
        fails.append(name)
check("bold", B in r("a **b** c") and "**" not in r("a **b** c"))
check("header", r("## T")[0:4] != "## T" and "##" not in r("## T") and B in r("## T"))
check("fence", "\x1b[36mcode line\x1b[0m" in r("```\ncode line\n```"))
check("fence-lang", "\x1b[36mx = 1\x1b[0m" in r("```python\nx = 1\n```"))
check("inline-code", "\x1b[32mcode\x1b[0m" in r("a `code` b"))
check("bullet", "•" in r("- item") and "- item" not in r("- item").replace("•", "X"))
check("quote", "│" in r("> cited"))
check("link", "txt" in r("[txt](http://x)") and "http://x" in r("[txt](http://x)") and "](" not in r("[txt](http://x)"))
check("plain", r("just text") == "just text")
live = m._MdLive()
live.render("```")
check("live-fence-state", live.render("x = 1") == "\x1b[36mx = 1\x1b[0m")
live.render("```")
check("live-fence-close", live.render("**b**") == B + "b" + "\x1b[0m")
if fails:
    print("renderer fails: %s" % " ".join(fails))
    sys.exit(1)
PYEOF
    ok "renderer unit"
else
    bad "renderer unit" "see above"
fi

# 13. streamed render: live ANSI out, raw markdown returned
clear_keys
if DANI_AI_API_KEY=t BASE="$BASE" python3 - "$AI" <<'PYEOF'; then
import io, os, sys
from importlib.machinery import SourceFileLoader
m = SourceFileLoader("daniai", sys.argv[1]).load_module()
buf = io.StringIO()
raw = m.complete(os.environ["BASE"], "t", "mm",
                 [{"role": "user", "content": "MDTEST live"}],
                 10, None, None, True, buf, True)
out = buf.getvalue()
assert raw == "**bold** and `code`", "raw=%r" % raw
assert "\x1b[" in out, "no ANSI in %r" % out
assert "**" not in out, "raw md leaked in %r" % out
assert "│" not in out and "┌─ 🤖" in out, "copy-safe frame missing in %r" % out
fr = m.frame_answer("hi", "🤖 · mm", width=40)
assert "🤖 · mm" in fr and "│" not in fr and "\nhi\n" in fr and "└" in fr, "frame %r" % fr
fw = m._FrameWriter(io.StringIO(), "T", width=20)
fw.write("ab"); fw.write("cd\nef"); fw.end()
got = fw.out.getvalue()
assert "│" not in got and "\nabcd\nef\n" in got, "framewriter %r" % got
PYEOF
    ok "stream render live"
else
    bad "stream render live" "see above"
fi

# 14. bare run (no args, EOF stdin) enters REPL and exits 0, no crash
clear_keys
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream < /dev/null 2>&1); rc=$?
if [ $rc -eq 0 ]; then ok "bare run no crash"; else bad "bare run no crash" "rc=$rc out=$out"; fi

# 15. models subcommand lists sorted ids
clear_keys
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" models 2>&1); rc=$?
if [ "$out" = "m-a
m-b" ] && [ $rc -eq 0 ]; then ok "models list sorted"; else bad "models list sorted" "rc=$rc out=$out"; fi

# 16. REPL /model bare shows list, pick by number
clear_keys
out=$(printf '/model\n2\nquit\n' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream chat 2>&1)
echo "$out" | grep -q "1\. m-a" && echo "$out" | grep -q "(model = m-b)" \
    && ok "repl /model pick" || bad "repl /model pick" "$out"

# 17. REPL /model <name> sets directly (no fetch)
clear_keys
out=$(printf '/model custom-x\nquit\n' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream chat 2>&1)
echo "$out" | grep -q "(model = custom-x)" \
    && ok "repl /model set" || bad "repl /model set" "$out"

# 18. /save writes session file + auto-saves last.json
clear_keys
out=$(printf 'hello save\n/save t1\nquit\n' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream chat 2>&1)
if [ -f "$EMPTY_CFG/daniwm/ai-history/t1.json" ] && grep -q "hello save" "$EMPTY_CFG/daniwm/ai-history/t1.json"; then
    ok "save writes file"
else
    bad "save writes file" "$out"
fi
if [ -f "$EMPTY_CFG/daniwm/ai-history/last.json" ]; then ok "auto-save last"; else bad "auto-save last" "missing"; fi

# 19. --continue resumes saved session
clear_keys
printf 'hello save\n/save t1\nquit\n' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream chat >/dev/null 2>&1
out=$(printf 'quit\n' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream chat --continue t1 2>&1)
echo "$out" | grep -q "resumed" \
    && ok "continue resumes" || bad "continue resumes" "$out"

# 20. history lists saved chats
clear_keys
printf 'hello save\n/save t1\nquit\n' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream chat >/dev/null 2>&1
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" history 2>&1)
echo "$out" | grep -q "t1" \
    && ok "history lists" || bad "history lists" "$out"

# 21. partial /command + Enter suggests, bare / lists
clear_keys
out=$(printf '/mod\n/\nquit\n' | DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-stream chat 2>&1)
echo "$out" | grep -q "did you mean /model?" \
    && ok "suggest partial command" || bad "suggest partial command" "$out"
echo "$out" | grep -q "commands: /base" \
    && ok "bare slash lists" || bad "bare slash lists" "$out"

# 22. completer unit (no tty needed)
clear_keys
if python3 - "$AI" "$EMPTY_CFG/daniwm/ai-history" <<'PYEOF'; then
import sys
from importlib.machinery import SourceFileLoader
m = SourceFileLoader("daniai", sys.argv[1]).load_module()
hd = sys.argv[2]
c = m.complete_token
assert c(hd, "/mod", "/mod", 0) == "/model", "tab /mod"
assert c(hd, "/mod", "/mod", 1) is None, "tab overflow"
assert c(hd, "/", "/", 0) == "/base", "tab / first"
assert c(hd, "hello", "hello", 0) is None, "no slash no complete"
assert m.suggest_command("/") .startswith("commands:"), "suggest /"
assert m.suggest_command("/mod") == "did you mean /model?", "suggest /mod"
assert m.suggest_command("/quit") is None, "exact known"
assert m.suggest_command("/xyz") is None, "unknown"
assert m.suggest_command("hi") is None, "not slash"
PYEOF
    ok "completer unit"
else
    bad "completer unit" "see above"
fi

# 23. custom tool manifest is advertised and executed via the real CLI
clear_keys
mkdir -p "$EMPTY_CFG/daniwm/ai.tools"
cat > "$EMPTY_CFG/daniwm/ai.tools/clock_probe.json" <<'JSONEOF'
{"name": "clock_probe", "description": "Return a fixed marker",
 "command": ["python3", "-c", "import json,sys; a=json.load(sys.stdin); print(json.dumps({'marker': 'CUSTOM-OK', 'echo': a.get('tag')}))"],
 "parameters": {"type": "object", "properties": {"tag": {"type": "string"}}, "additionalProperties": false}}
JSONEOF
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --show-config 2>&1)
echo "$out" | grep -q "clock_probe" && echo "$out" | grep -q "custom_tools = clock_probe" \
    && ok "custom tool advertised" || bad "custom tool advertised" "$out"
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --no-custom-tools --show-config 2>&1)
case "$out" in
    *clock_probe*) bad "--no-custom-tools hides tool" "$out" ;;
    *) ok "--no-custom-tools hides tool" ;;
esac
# invalid manifest is reported but does not break startup
echo '{"name": "Bad Name", "description": "x", "command": ["true"]}' > "$EMPTY_CFG/daniwm/ai.tools/bad.json"
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --show-config 2>&1); rc=$?
echo "$out" | grep -q "tool_manifest_error = bad.json" && [ $rc -eq 0 ] \
    && ok "invalid manifest reported" || bad "invalid manifest reported" "rc=$rc out=$out"
rm -f "$EMPTY_CFG/daniwm/ai.tools/bad.json"

# 24. BTC market tool ships valid manifest and runs (network optional)
if python3 - "$AI" "$ROOT/ai.tools" <<'PYEOF'; then
import json, subprocess, sys, tempfile
from importlib.machinery import SourceFileLoader
m = SourceFileLoader("daniai", sys.argv[1]).load_module()
tool = m.CustomTool(sys.argv[2] + "/btc_market.json")
assert tool.name == "btc_market" and tool.confirm is False
assert tool.command[1].endswith("btc_market.py")
assert m.validate_json_value({"interval": "4h", "limit": 10}, tool.parameters) is None
assert m.validate_json_value({"interval": "3h"}, tool.parameters)
assert m.validate_json_value({"nope": 1}, tool.parameters)
script = tool.command[1]
proc = subprocess.run([sys.executable, script], input=b'{"interval":"2h"}',
                      capture_output=True)
if proc.returncode == 0:
    data = json.loads(proc.stdout)
    assert data.get("symbol") == "BTCUSDT"
    assert "klines" in data and data["klines"]["interval"] == "2h"
else:
    print("btc tool skipped network check (rc=%d)" % proc.returncode)
proc = subprocess.run([sys.executable, script], input=b'{"symbol":"BTC USDT"}',
                      capture_output=True)
assert proc.returncode == 2 and "error" in json.loads(proc.stdout)
PYEOF
    ok "btc_market tool"
else
    bad "btc_market tool" "see above"
fi

# 25. full CLI round-trip: model calls a custom tool, result is sent back
clear_keys
out=$(DANI_AI_API_KEY=t "$AI" --api-base "$BASE" --tools-dir "$EMPTY_CFG/daniwm/ai.tools" \
      "CALLTOOL now" 2>&1); rc=$?
if [ "$out" = "tool-answer" ] && [ $rc -eq 0 ]; then ok "custom tool CLI round-trip"; else bad "custom tool CLI round-trip" "rc=$rc out=$out"; fi
if grep -q "CUSTOM-OK" "$MOCK_LAST"; then ok "custom tool result sent back"; else bad "custom tool result sent back" "$(cat "$MOCK_LAST")"; fi

if python3 "$TDIR/test-ai-regression.py"; then
    ok "security and failure-path regressions"
else
    bad "security and failure-path regressions" "see above"
fi

echo "---"
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
