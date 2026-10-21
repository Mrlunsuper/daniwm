# Tiếp tục dani-ai

## Trạng thái bàn giao

User đã yêu cầu commit và push phần dani-ai sau đợt 12. Chỉ đưa các file AI liệt kê bên dưới vào commit; giữ nguyên các thay đổi WM/launcher không liên quan. Các lần commit tiếp theo vẫn cần user yêu cầu.

Đọc `AUDIT-DANI-AI.md` để xem findings và 12 đợt triển khai; README mô tả flags mới.

## Đã triển khai

1. Journal checkpoint trước/sau tool, giữ kết quả khi request lỗi/hủy; pending result ghi execution status unknown.
2. Session v2 lưu root/provider/state/time; resume mismatch/legacy cần `--trust-session`; không đổi provider qua `/base` trong REPL.
3. `--permissions ask` mặc định, diff approval; `read-only`, `workspace-write`, `--yes-tools` không vượt blocklist/read-only.
4. API/stream 16 MiB, SSE line 1 MiB/100k lines, answer/stdin 2 Mi ký tự, request/session 32 MiB; deadline body reader, web redirect dùng chung budget.
5. `--max-tool-calls` (64), `--max-tool-output` (1 MiB); `/context` chỉ báo bytes/messages, không giả là token usage.
6. Scan bỏ dependency/build/cache, `.aiignore` glob đơn giản, `--include-ignored`.
7. BTC tool finite validation, response cap 2 MiB, budget 24s, partial results, timestamps/age/candle state, funding annualization assumption.
8. `--no-history` không persist/load history/checkpoint, chặn save/resume/continue; slash commands mới không thêm readline history; history list không cần key.
9. `make check-ai` và tích hợp make check; `.github/workflows/ai.yml` matrix Python 3.10/3.12/3.14.
10. Session ID UUID riêng, resume branch không sửa nguồn; `flock` serialize ID/last/save, `/save` từ chối overwrite, readline merge entry mới. Reset/new/clear/đổi root đổi ID. `last` vẫn là latest checkpoint giữa mọi phiên; dùng ID cụ thể để resume chính xác. Chưa retention/cleanup; ID + last không crash-atomic như một transaction.

11. Provider profiles opt-in (`--profile`, `--profiles-file`): pin base/model/key_env không fallback; capabilities tools/vision/stream/token parameter; session context gắn profile, khóa đổi model trong REPL. Remote HTTP cảnh báo. Chưa retry/backoff hay auto-discovery.

12. `/usage` provider-reported token counts + per-field coverage, process-only, không estimate cost; `--stream-usage` opt-in SSE usage request. `/compact [N=4]` bỏ nguyên lượt cũ, giữ system/tool pairs; backup ID gốc rồi branch, checkpoint lỗi giữ RAM; private không backup. Không model summary/auto-compact.

13. Builtin path/domain policy: `--deny-path`, `--write-path`, `--allow-domain` (repeatable); deny cả ancestor, lọc list/scan, write allowlist; exact hostname allowlist kiểm tra mỗi redirect trước DNS. Ask approval cache theo origin/session, xóa khi new/reset/clear/resume/đổi root. Không sandbox shell/custom, không lọc metadata git_status hay scrub history cũ.

14. `/diff`/`/undo` latest user turn cho builtin write/edit, RAM snapshot 8 MiB/turn; undo content/mode hoặc xóa file mới. Preflight content+identity, nofollow, policy, confirmation; từ chối user/intervening edits hoặc shell/custom execution. Conversation có notice file đã restore. Không mkdir/restart/single-shot undo, không transaction đa file hay chống hostile concurrent writer.

15. `--turn-timeout` cooperative shared budget 300s mặc định cho API/tool rounds/web/subprocess/retry; không hard wall-clock DNS/connect/header/fs/lock/approval. `--http-retries` opt-in 0..3 mặc định 0, chỉ HTTP 429/502/503/504 trước body, Retry-After/backoff trong budget; không transport/body/partial-stream/tool replay. Test mutation chạy đúng một lần qua provider retry.

16. Scan cursor/pagination find_files/search_files: next_cursor/single-use opaque process-only, resume fd-relative DFS, 2000 entries/100 matches/16000 chars mỗi page, long lines clip. Max 4 active, lazy idle TTL 120s, parameter/policy invalidation; reset/resume/root/exit đóng fd. Không snapshot FS/sorted order/full gitignore.

## Kiểm tra gần nhất

- `make check-ai`: **35 PASS, 0 FAIL; 100 Python regression tests OK**.
- `python3 -m py_compile dani-ai ai.tools/btc_market.py test/test-ai-regression.py`: pass.
- `git diff --check`: pass.
- Chỉ mock/local HTTP, chưa dùng provider hay Binance thật. CI matrix chưa chạy trên GitHub. Chưa chạy full WM suite.

## Các file thuộc công việc AI

- `dani-ai`
- `ai.tools/btc_market.py`
- `test/test-ai-regression.py`, `test/test-ai.sh`
- `README.md`, `ai.system.example`
- `Makefile`
- `.github/workflows/ai.yml` (mới)
- `AUDIT-DANI-AI.md` (mới)
- `DANI-AI-RESUME.md` (file này)

## Quan trọng: giữ nguyên thay đổi không liên quan

Working tree từ trước có thay đổi WM/launcher: `run.config`, `src/client.c`, `src/keys.c`, `src/layout.c`, `src/run.c`, `test/run-all.sh`, `test/test-tasklist.sh`, `test/test-workspaces.sh`; cùng nhiều file audit/test WM/run untracked. Không revert/stage/commit các file này theo công việc AI nếu chưa được user yêu cầu. Luôn xem lại `git status` và diff hiện tại khi tiếp tục.

## Phần còn lại / thứ tự đề xuất

Đã hoàn thành session ID + concurrency locking, provider profiles/capabilities, context compaction/usage, builtin path/domain policy, `/diff`/`/undo` RAM-only và cooperative turn deadline/retry opt-in. Đã thêm scan cursor/pagination. Ưu tiên tiếp theo đề xuất: **OI history/multi-timeframe** hoặc session export/delete UX. User chưa chọn cụ thể bước tiếp theo.

- Session ID + advisory locking đã có; `last.json` cố ý là latest snapshot chung, không phải identity. Cần retention/cleanup/export/delete nếu tiếp tục mở rộng session UX.
- Path/domain policy CLI đã có; chưa policy file riêng hay URL path rules. Không coi policy/prompt là sandbox hay DLP; shell/custom và git_status metadata không được lọc theo deny-path.
- `/diff`/`/undo` đã có RAM-only latest turn; chưa persistent journal/crash recovery, mkdir undo hoặc multi-file transaction. Không reset toàn repo; tránh writer ngoài sửa đồng thời lúc undo.
- Cooperative total-turn deadline và bounded retry đã có; DNS/connect/response-header/fs/lock/approval chưa có hard wall-clock deadline. Retry chỉ opt-in HTTP transient status, không transport ambiguity.
- Compaction đã có dạng bỏ nguyên lượt cũ; chưa model summary/auto-threshold. Usage đã có process-only provider counts; chưa persistent ledger/cost có bảng giá cấu hình. Profile đã có base/key_env/model/capabilities cố định; chưa auto-discovery hay switch trong REPL.
- Scan cursor/pagination đã có; chưa đầy đủ `.gitignore` (hiện chỉ `.aiignore` glob đơn giản) hoặc stable FS snapshot/sorted scan.
- OI history/multi-timeframe market analysis.
- `/retry`, `/edit-last`, branching, export/delete, attach/editor, JSONL, read-only parallelism nếu có nhu cầu.

## Ghi chú khi rà soát tiếp

- Private mode cố ý không có disk crash recovery; provider vẫn nhận dữ liệu và workspace tools vẫn có thể ghi theo quyền.
- Khi tool output vượt budget, tool đã chạy; không được tự replay mutation.
- Session warning cho interrupted/in_progress là chủ đích.
- Test import chính và market tools dùng exec_module; một vài subprocess test snippets có thể còn legacy import loader, kiểm tra nếu nâng Python.
- Test lại `make check-ai` sau mọi thay đổi và cập nhật audit/handoff theo trạng thái thực.
