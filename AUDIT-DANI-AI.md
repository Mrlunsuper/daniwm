# Audit dani-ai

## Phạm vi và cách kiểm tra

- Đọc `dani-ai`, `ai.system.example`, hai market tool trong `ai.tools/`, README liên quan, cấu hình test/CI và test regression hiện có.
- Chạy `./test/test-ai.sh`: **35 PASS, 0 FAIL**, regression **55 tests OK**.
- Tái hiện bằng import module, mock API và thư mục tạm; không gọi provider thật, không dùng API key thật.
- Không sửa mã nguồn. Không đánh giá chất lượng câu trả lời của model hay latency provider thực tế.
- Severity dưới đây phản ánh CLI có tool ghi bật mặc định; shell vẫn tắt mặc định và được mô tả rõ là không sandbox.

## Điểm đang làm tốt

- Python stdlib, triển khai nhẹ, không cần dependency runtime bên ngoài cho chat thông thường.
- Shell opt-in; subprocess có giới hạn output, deadline và cleanup process group.
- File tool dùng descriptor-relative traversal, O_NOFOLLOW, chặn traversal/symlink/hardlink; write create-only và edit publish atomic.
- Session ghi atomic với mode riêng tư; protocol validation và incomplete-stream handling khá tốt.
- Chặn redirect khác origin để bảo vệ Authorization; fetch URL có SSRF checks và kiểm tra peer thực tế.
- Có regression test cho lỗi transport, terminal injection, clipboard, tools và persistence.

## Phát hiện cần xử lý

### A1 — High: mất dấu tác động tool khi lượt chat lỗi

**Vị trí:** `dani-ai:1469–1505`, `2362–2376`.

`tool_complete` chỉ ghi lịch sử tool từ `work` vào `messages` sau khi có câu trả lời cuối thành công. Nếu tool đã tạo/sửa file rồi request kế tiếp lỗi hoặc người dùng Ctrl-C, các thao tác vẫn tồn tại nhưng lịch sử tool biến mất; REPL còn pop user message.

**Đã tái hiện:** mock một `write_file` thành công, request sau ném AIError: file tồn tại nhưng `messages` không chứa tool result nào.

**Đề xuất:** journal tool call/result ngay sau thực thi; lưu trạng thái turn interrupted/failed. Báo rõ thao tác nào đã chạy, không ngụ ý lỗi request đồng nghĩa không có thay đổi. Không tự retry lại thao tác có side effect. Có checkpoint trước mutation và cơ chế diff/undo riêng, không tự reset toàn repo.

### A2 — High: session không gắn với project/provider

**Vị trí:** `dani-ai:106–152`, `2158–2175`, `2251–2260`, `2439–2444`.

Session chỉ có `model` và `messages`, không lưu tool root hay provider origin. `/root` đã chủ động xóa hội thoại để tránh stale context, nhưng `/resume` và `--continue` đưa nguyên tool history cũ vào root hiện tại mà không kiểm tra. Model có thể chỉnh project B dựa trên nội dung project A. Đổi provider qua config hoặc `/base` còn có thể gửi toàn bộ hội thoại cũ sang nơi khác.

**Đề xuất:** session schema có version, root canonical, provider/profile ID, timestamps; yêu cầu xác nhận khi root/origin không khớp. Cho lựa chọn chỉ nhập text hoặc bỏ tool context. Không lưu API key. Namespace `last` theo project hoặc dùng session ID độc lập.

### A3 — High: quyền ghi + network mặc định chưa có policy gate

**Vị trí:** `dani-ai:910–959`, `999–1003`, `1207–1238`, `1305–1384`.

`write_file`, `edit_file`, `make_directory`, `fetch_url` đều bật mặc định và không hỏi xác nhận. Nội dung file/web là dữ liệu không tin cậy, nhưng nhãn `untrusted` và system prompt không phải ranh giới bảo mật. Model vẫn có thể bị dẫn dắt sửa file hoặc đưa nội dung đọc được vào query/path của URL công khai. Bộ lọc tên nhạy cảm là heuristic, không bảo đảm chặn secret.

**Đã kiểm tra:** các tên `service-account.json`, `kubeconfig`, `private.ppk`, `backup.sql`, `config.toml` đều được `allowed()` chấp nhận. Đây không tự chứng minh chúng chứa secret, nhưng cho thấy giới hạn của heuristic.

**Đề xuất:** preset `read-only`, `ask`, `workspace-write`; mặc định an toàn hơn cho dữ liệu lạ. Preview diff trước ghi, permission theo tool/path/domain và xác nhận egress tới domain mới. Cho phép người dùng khai báo sensitive globs; không quảng cáo cơ chế này là sandbox hay DLP hoàn chỉnh. Custom tool cần trust riêng, không suy ra là an toàn chỉ vì chạy argv.

### A4 — Medium: transport giới hạn idle timeout nhưng thiếu tổng deadline/size budget

**Vị trí:** `dani-ai:340–376`, `448–519`, `1517–1519`, `1549–1550`, `1579–1648`, `2145–2152`.

API JSON/models và stdin được đọc toàn bộ không có size cap. SSE không giới hạn byte/line, tổng content, tổng tool ID/name; timeout socket không phải deadline toàn request. Web download có byte cap nhưng cũng chưa có tổng wall-clock deadline, DNS resolution chưa có deadline riêng. Server nhỏ giọt có thể giữ kết nối lâu.

**Đề xuất:** total deadline, connect/read idle timeout tách biệt; cap response/event/content/stdin/session; trả lỗi rõ. Hạn chế độ dài mọi trường stream, không chỉ tool arguments. Thêm test slow stream, event lớn, nhiều event và input lớn.

### A5 — Medium: context và chi phí tăng không kiểm soát

**Vị trí:** `dani-ai:1471–1505`, `2155–2389`, `1564–1651`.

Mỗi vòng gửi lại toàn bộ lịch sử, tool output và ảnh base64; không theo dõi usage/context budget, không compact. Tối đa 32 vòng thực thi × 16 tool calls/vòng, chạy nối tiếp. Không có budget tổng thời gian/chi phí của một lượt.

**Đề xuất:** `/usage`, `/context`, `/compact`, giới hạn tổng tool calls/thời gian/output mỗi lượt; giữ nguyên cặp tool call/result khi compact. Ảnh cũ có chính sách retention rõ. Usage/cost chỉ tính khi provider trả số liệu và có bảng giá do người dùng cấu hình.

### A6 — Medium: tìm kiếm dễ tốn budget vào dependency/build artifacts

**Vị trí:** `dani-ai:1045–1104`, `1357–1379`.

Không đọc `.gitignore` hay có ignore list cho `node_modules`, build, vendor, cache. Scan dừng sau 2.000 entry, thứ tự theo filesystem nên kết quả phụ thuộc cây thư mục. Search bỏ file >64 KiB; phân trang read_file mỗi lần lại đọc/decode cả file tới 2 MiB.

**Đã tái hiện:** `find_files` trả `node_modules/dependency.js` dù `.gitignore` đã ignore `node_modules/`.

**Đề xuất:** ignore mặc định + `.aiignore`, tùy chọn include ignored; pagination/cursor cho scan; ưu tiên source. Nếu tích hợp Git ignore hoặc rg, phải giữ nguyên các kiểm tra root/symlink và fallback stdlib. Tăng giới hạn có chủ đích, không chỉ bỏ mọi cap. Cache đọc chỉ với invalidation theo metadata hoặc hash phù hợp.

### A7 — Medium: khả năng phục hồi và tương thích provider còn thủ công

**Vị trí:** `dani-ai:243–267`, `1508–1651`, `2334–2341`.

Không có retry/backoff cho 429/502/503 hoặc hỗ trợ Retry-After; không có profile key/base/model/capabilities. `/base` đổi endpoint nhưng giữ key và toàn bộ hội thoại. Tool schemas/max_tokens được gửi theo một kiểu chung, provider không hỗ trợ phải dùng flag thủ công.

**Đề xuất:** profiles gắn key với origin, cảnh báo endpoint HTTP không phải loopback và xác nhận đổi origin. Capability cấu hình được cho tools/vision/token parameter. Retry có giới hạn chỉ trước side effect hoặc trước khi xuất partial stream; không tự replay tool đã chạy. Giữ nguyên chặn redirect khác origin.

### A8 — Medium: chưa tích hợp test AI vào gate chung/CI

**Vị trí:** `Makefile:99–101`, `test/run-all.sh:7`, `.github/workflows/pages.yml`.

Test AI chạy tốt khi gọi riêng, nhưng `make check`/run-all hiện không gọi `test-ai.sh`; workflow hiện có chỉ phục vụ Pages. Regression test còn dùng `SourceFileLoader.load_module()` đã deprecated, cảnh báo sẽ bị gỡ trên Python 3.15.

**Đề xuất:** `make check-ai` và CI headless riêng, matrix phiên bản Python được hỗ trợ; chuyển test import sang `spec_from_loader`/`exec_module`. Thêm test cho A1/A2/A4, concurrent sessions, provider mismatch và slow network.

### A9 — Medium: market tool BTC thiếu metadata/chống dữ liệu sai ở vài điểm

**Vị trí:** `ai.tools/btc_market.py:20–32`, `88–146`, `ai.tools/btc_market.json:5`.

- `number()` nhận NaN/Infinity, không chặn bool như tool vàng.
- Response HTTP đọc không có byte cap và một số shape sai có thể gây AttributeError/TypeError không được xử lý.
- Năm section gọi nối tiếp với timeout 10s mỗi request nhưng manifest chỉ cho tổng 30s: khi mạng chậm có thể bị kill và mất cả kết quả đã có.
- `annualized_pct` cố định giả định funding 3 lần/ngày; không đúng cho mọi USDT-M instrument mà tool cho phép nhập.
- Nến chưa có close time/is_closed; timestamp unit và freshness chưa rõ như tool vàng; OI chỉ snapshot, không đủ kết luận xu hướng OI.

**Đề xuất:** finite numeric validation, bounded response và schema checks, total deadline trả partial results, funding interval thực tế hoặc ghi rõ giả định. Thêm close_time/is_closed, timestamp_unit, age và OI history. Multi-timeframe là nâng cấp hữu ích sau khi dữ liệu nền đáng tin.

### A10 — Low: persistence/UX còn thiếu các chế độ cơ bản

**Vị trí:** `dani-ai:2122–2142`, `2378–2389`, `2491–2500`.

Autosave session chỉ khi thoát; crash/kill có thể mất hội thoại. Nhiều REPL cùng ghi `last.json` và readline history gây last-writer-wins. Readline lưu cả lệnh/prompt, có thể gồm `/system` chứa thông tin riêng tư. `history` bị kiểm tra API key trước dù là thao tác local.

**Đề xuất:** autosave mỗi lượt hoàn tất và journal sau tool, session ID + lock/merge phù hợp; `--no-history`/private mode và history filtering; list history không cần key. Export Markdown/JSON và xóa session có xác nhận.

## Lộ trình đề xuất

### P0 — An toàn và nhất quán

1. Tool journal + interrupted-turn state, test lỗi sau mutation.
2. Session schema v2 gắn root/provider, mismatch gate, migration session cũ.
3. Permission presets, diff approval và domain trust; giữ shell opt-in.
4. Deadline/byte budget toàn request và input.

### P1 — Tối ưu sử dụng hằng ngày

1. Usage/context budget, `/compact`, tool budget.
2. Ignore-aware search, scan pagination; an toàn trước khi tăng tốc.
3. Provider profiles/capabilities, bounded retry.
4. Per-turn autosave, private mode và CI AI riêng.

### P2 — Tính năng nên bổ sung

- `/diff`, `/undo` theo checkpoint của lượt chat, không reset thay đổi user.
- `/retry`, `/edit-last`, session branching; retry không lặp side effect.
- `--format json` hoặc JSONL event stream cho scripting; tách progress/tool diagnostics khỏi stdout answer.
- `/attach <file>` cho text/image, multiline/editor input; preview dữ liệu sắp gửi.
- Tool reload/status trong REPL và quyền theo session.
- Đọc song song có giới hạn sau khi có journal; mutation giữ thứ tự, không parallel tùy tiện.
- BTC multi-timeframe/OI history và phân tích deterministic có test; tránh chỉ giao mọi phép tính cho LLM.

## Triển khai đợt 1

Đã xử lý nền tảng A1/A2/A3:
- Tool history được giữ khi request tiếp theo lỗi; checkpoint trước/sau tool, pending result ghi rõ trạng thái không biết nếu bị ngắt.
- Session v2 có root/provider, turn state, updated timestamp; resume mismatch/legacy yêu cầu `--trust-session`. Chặn đổi provider qua `/base` trong REPL để không dùng lại key nhầm endpoint.
- CLI mặc định `--permissions ask`, preview diff trước ghi; thêm `read-only` và chế độ tương thích `workspace-write`. Read-only chặn shell/network/custom, kể cả khi auto-approve.
- REPL autosave mỗi lượt, single-shot có tool cũng checkpoint. README/system prompt được cập nhật.
- Xác minh: `./test/test-ai.sh`: **35 PASS, 0 FAIL; 60 regression tests OK**; `py_compile` và `git diff --check` pass.

Giới hạn còn lại: chưa có checkpoint rollback/undo, trust theo domain/path, session ID/locking cho nhiều REPL, full deadline/size budgets hoặc CI mới. Diff quá dài được gắn cảnh báo truncation. Policy này không phải sandbox/DLP; `--yes-tools` là opt-in bỏ prompt, không vượt read-only/blocklist. Các mục P1/P2 khác chưa được triển khai trong đợt này.

## Triển khai đợt 2

- API response/stream giới hạn 16 MiB, SSE line 1 MiB và 100.000 dòng, answer 2 Mi ký tự; request/session 32 MiB, stdin 2 Mi ký tự. Session save quá budget không ghi đè bản cũ.
- Body reader dùng read1 và giảm socket timeout theo budget còn lại; web redirect/body dùng chung deadline. Đã test server local gửi nhỏ giọt.
- Giới hạn tool name/id cho cả stream và nonstream; partial answer vượt budget không được lưu.
- `make check-ai`, tích hợp vào `make check`, workflow CI matrix Python 3.10/3.12/3.14. Test import chuyển sang exec_module ở module chính và market tools.
- Xác minh local: **35 PASS, 0 FAIL; 65 regression tests OK**; py_compile và git diff --check pass. Chưa chạy GitHub CI matrix thực tế.
- Deadline chưa phải hard wall-clock cho DNS resolver và urllib connect/response-header phase. Chưa triển khai context compaction, total-turn budget, ignore-aware search hay các mục market/profile/undo còn lại.

## Triển khai đợt 3

- Scan loại dependency/build/cache mặc định; hỗ trợ `.aiignore` glob đơn giản ở tool root và `--include-ignored`. Mở ignore config qua fd/no-follow, không nới sensitive-path/symlink/hardlink checks. Chưa hỗ trợ đầy đủ `.gitignore` semantics hay scan pagination.
- Per-turn budget 64 tool calls và 1 MiB tool result, điều chỉnh qua CLI. Batch vượt call budget không chạy; output vượt budget ghi rõ tool đã chạy và ngừng các call còn lại. Không rollback/retry tự động.
- `/context` báo message/tool count và JSON UTF-8 bytes, không giả nhận đó là token usage hay billing. Chưa tự compact history hoặc đặt total-turn wall-clock deadline.
- Xác minh `make check-ai`: **35 PASS, 0 FAIL; 68 regression tests OK**; py_compile và git diff --check pass.

## Triển khai đợt 4

- BTC numeric conversion từ chối bool/NaN/Infinity; kiểm tra shape theo section và các argument không hợp lệ.
- Giới hạn 2 MiB/HTTP response, stdin 64 Ki ký tự; budget dữ liệu 24s, trả partial sections/errors thay vì mất mọi kết quả. DNS/connect/header vẫn không có hard wall-clock guarantee.
- Thêm timestamp unit, completed_at, quote age khi có source time, close_time/is_closed cho nến. Annualized funding ghi rõ giả định 8 giờ/constant rate, không coi là lịch thực hay dự báo.
- Chưa bổ sung OI history hoặc multi-timeframe analysis.
- `make check-ai`: **35 PASS, 0 FAIL; 71 regression tests OK**; py_compile và git diff --check pass; không gọi API thị trường thật.

## Triển khai đợt 5

- `--no-history` không load/save readline, session hay tool checkpoint; chặn save/resume/continue trong private mode. Tool history vẫn giữ trong bộ nhớ, không có crash recovery trên đĩa. Không đồng nghĩa offline hoặc sandbox.
- Slash-command không thêm vào readline history mới; chưa scrub lịch sử cũ. Session bình thường vẫn lưu system prompt.
- Local `history` không yêu cầu API key; private mode không đọc session để list.
- `make check-ai`: **35 PASS, 0 FAIL; 74 regression tests OK**. Chưa xử lý concurrent session ID/locking hoặc export/delete UX.

## Triển khai đợt 6

- Mỗi REPL/tool-checkpoint single-shot dùng `session-<uuid>.json` riêng, metadata có `session_id`; resume tạo nhánh mới, không sửa nguồn. Reset/new/clear/đổi root bắt đầu ID mới.
- Serialize publish ID + `last.json` bằng advisory `flock`; `last` giữ tương thích và nghĩa là checkpoint vừa publish gần nhất. Muốn chọn đúng phiên concurrent phải dùng ID cụ thể. Lock regular/single-link/no-follow, mode 0600; kernel tự giải phóng khi process chết.
- `/save` không ghi đè tên đã tồn tại; kiểm tra và save trong lock. Readline merge entry mới với lịch sử mới nhất trong lock, giữ giới hạn 200 entry thay vì ghi đè snapshot cũ.
- Test 4 process × 12 checkpoint, merge hai REPL stale snapshot, branch/reset/save collision, lock release/symlink. `make check-ai`: **35 PASS, 0 FAIL; 78 regression tests OK**; py_compile và git diff --check pass.
- Lock chỉ dành cho cooperating writers/filesystem hỗ trợ flock, không phải sandbox. ID và last là hai atomic writes riêng, không phải transaction chống crash; ID có thể mới hơn last nếu process chết giữa hai publish. Chưa có retention/cleanup/export/delete; chưa chạy provider/Binance thật, GitHub CI hay full WM suite.

## Triển khai đợt 7

- `--profile NAME` + `--profiles-file` (mặc định `~/.config/daniwm/ai.profiles.json`), schema version 1, input cap 1 MiB, validation endpoint/key-env/capabilities. Không chọn profile giữ nguyên resolution cũ.
- Profile pin base/model/key_env; chỉ lấy key từ biến môi trường chỉ định, không fallback. Chặn override base/key và model khác profile. Request chỉ tới base profile, redirect khác origin vẫn chặn; remote HTTP cảnh báo plaintext transport. File profile là cấu hình tin cậy, không phải bảo vệ khỏi người có quyền sửa config.
- Capabilities tools/vision/stream và token_parameter (`max_tokens`, `max_completion_tokens`, null) áp dụng chung stream/nonstream. Tắt runner/nonstream tự động theo capability, chặn ảnh trước request nếu không hỗ trợ. Không probe capabilities tự động.
- Session context lưu profile name/model/capabilities nhưng không lưu key. Resume model khác profile bị từ chối, `/model` bị khóa trong profile; context mismatch vẫn cần trust gate.
- Thêm test key isolation/no-fallback, override/config validation, capability guards, stream/nonstream payload và session mismatch. `make check-ai`: **35 PASS, 0 FAIL; 82 regression tests OK**; py_compile và git diff --check pass.
- Chưa retry/backoff, dynamic profile switch, provider-reported usage hay compaction. Chỉ mock/local HTTP; chưa provider thật/CI/full WM suite.

## Triển khai đợt 8

- `/usage` báo provider-reported prompt/completion/total tokens, coverage riêng từng trường, tổng trong process và last response; không suy diễn số thiếu thành zero, không ước tính billing/cost. Bao gồm các tool rounds và title requests; không persist usage qua resume. Stream thiếu DONE không ghi nhận là completed, vẫn cảnh báo có thể bị billing.
- Thu thập usage nonstream và SSE usage-only event. `--stream-usage` opt-in request `stream_options.include_usage`; không tự bật để tránh phá provider không hỗ trợ, không retry/fallback tự động.
- `/compact [N]` giữ N user turns gần nhất (default 4) và system prompts; bỏ nguyên lượt cũ, giữ toàn bộ tool-call/result trong lượt được giữ. Không summary bằng model, không auto-compact. Có thể mất thông tin lịch sử cũ theo chủ ý của người dùng.
- Trước compact checkpoint bản gốc theo ID, publish context gọn sang ID mới rồi mới đổi RAM; lỗi checkpoint giữ nguyên context RAM. Private mode không ghi đĩa, cảnh báo không backup.
- Test usage valid/missing/malformed, stream-only usage, option payload, incomplete stream, tool-pair integrity, compaction branch/private/local commands và checkpoint failure. `make check-ai`: **35 PASS, 0 FAIL; 86 regression tests OK**; py_compile và git diff --check pass.
- Chưa cost pricing, persistent usage ledger, model-based summary, auto context threshold, retry/backoff hay total-turn deadline. Không gọi provider/Binance thật, chưa GitHub CI/full WM suite.

## Triển khai đợt 9

- `--deny-path GLOB` (repeatable) chặn builtin read/write/mkdir, lọc list/find/search, kiểm tra cả ancestor. `--write-path GLOB` giới hạn builtin mutations nếu được cấu hình; mkdir kiểm tra từng parent. fnmatch trên root-relative normalized path, không phải gitignore; deny ưu tiên hơn allow, không bypass bằng include-ignored/auto-approval.
- `--allow-domain HOST` exact IDNA/case-normalized hostname allowlist cho fetch_url, kiểm tra trước DNS ở mỗi hop redirect. SSRF/peer/HTTPS-downgrade checks cũ giữ nguyên. Preset ask xác nhận mỗi origin mới một lần trong session, bao gồm redirect; reset/new/clear/resume/đổi root xóa origin approval.
- Thêm show-config policy. Test denied reads/mutations/scan/list, allowed writes/mkdir, validation/traversal, origin cache/decline/read-only và redirect chặn trước DNS.
- `make check-ai`: **35 PASS, 0 FAIL; 89 regression tests OK**; py_compile và git diff --check pass. Chỉ mock/local HTTP, chưa provider/Binance thật/CI/full WM suite.
- Giới hạn: builtin policy không sandbox shell/custom, không lọc metadata git_status, không scrub nội dung đã có trong session hoặc giới hạn API provider endpoint. Chưa policy file riêng/per-domain URL path rules hoặc tổng deadline/undo.

## Triển khai đợt 10

- `/diff` và `/undo` theo lượt user gần nhất cho builtin write_file/edit_file; snapshot trước lần đầu và sau lần cuối mỗi path, RAM-only, budget 8 MiB mutation snapshot/turn. New turn/reset/new/clear/resume/root change xóa snapshot. Không undo mkdir/shell/custom/restart/single-shot.
- Undo preview/confirmation, tuân thủ read-only/blocklist/path policy; preflight mọi file bằng content + inode/timestamps/mode/size/nlink, mở fd no-follow. Từ chối nếu user sửa hoặc có intervening edits giữa hai AI writes; shell/custom execution đánh dấu turn không undo được.
- Khôi phục nội dung/mode hoặc unlink file mới do AI tạo, không reset Git/repo. Check lại trước publish; ghi thông báo đường dẫn đã restore vào conversation và checkpoint để tránh dùng tool results stale. Snapshot không persist kể cả private mode.
- Test multi-edit aggregation, mode restore/create deletion, preflight refusal preserving other files, intervening edits, symlink, policy/budget và REPL notice/local commands. `make check-ai`: **35 PASS, 0 FAIL; 93 regression tests OK**; py_compile và git diff --check pass.
- Giới hạn: không transaction đa file, I/O failure có thể restore một phần. Check-before-publish không phải CAS filesystem, vẫn có race với concurrent external writer; không bảo đảm trước hostile concurrent writers. Chưa persistent undo journal/crash recovery hoặc total-turn deadline/retry.

## Triển khai đợt 11

- `--turn-timeout` (default 300s, finite positive) cooperative shared budget cho complete turn: request/tool rounds, retry sleeps, web và subprocess timeouts. Gate trước tool/request và sau approval trước file publication. Checkpoint không bị deadline chặn để giữ dấu tác động tool.
- `--http-retries` opt-in 0..3 (default 0), chỉ HTTP 429/502/503/504 trước response body; exponential backoff hoặc Retry-After seconds/HTTP-date, phải vừa request/turn budget. Không retry transport ambiguity/body/JSON/partial stream/tool/web; provider có thể bill duplicate POST work.
- Retry request sau tool không replay tool: test thực hiện một write, provider 503 rồi success, tool execute đúng một lần. Thêm test Retry-After/count/delay bounds/no transport retry, deadline giữa tool batch giữ result, expired body/subprocess gate và approval quá hạn không publish file.
- `make check-ai`: **35 PASS, 0 FAIL; 97 regression tests OK**; py_compile và git diff --check pass.
- Deadline không hard wall-clock: DNS/connect/header/approval/filesystem/lock vẫn có thể vượt deadline đến khi control quay lại; không kill thread/process giữa file publication. Chưa jitter/adaptive retry hoặc provider thật/CI/full WM suite.

## Triển khai đợt 12

- find_files/search_files có optional cursor + next_cursor, resume generator descriptor-relative DFS thay vì quét lại prefix. Giữ cap 2000 scanned entries/100 matches/16000 result chars mỗi page; pending event giữ match/entry tại biên trang, search tiếp tục giữa các dòng cùng file.
- Cursor opaque UUID single-use, process-only, max 4 active scans, idle TTL 120s (lazy cleanup); evict oldest; signature root/params/ignore/deny/include-ignored. Reset/new/clear/resume/root change/REPL exit đóng generators/fds.
- Giữ nofollow/regular-singlelink/sensitive/deny/ignore/byte/depth checks; cooperative deadline trong scan loop. Long matching line clip có flag/count để không gây loop cursor không tiến. Depth-limited branch báo riêng, không continuation qua depth cap.
- Test đủ 2050 files qua match/entry budgets, same-file 240 matches + long line, single-use/expiry/policy invalidation/eviction/cleanup. `make check-ai`: **35 PASS, 0 FAIL; 100 regression tests OK**; py_compile và git diff --check pass.
- Cursor không snapshot filesystem, thứ tự vẫn theo filesystem, concurrent changes/rename có thể gây stale/missing results. Chưa full gitignore semantics hoặc stable sorted snapshot; chỉ mock/local tests, chưa CI/full WM suite/provider/Binance thật.

## Triển khai đợt 13

- BTC tool thêm `intervals` opt-in (1..4 unique frames, chứa primary interval, tổng <=400 candles). `timeframes` có window/all-candle và closed_window; giữ klines/window primary tương thích. Các frame lỗi độc lập, trả partial cùng interval và dùng chung budget 24s.
- `open_interest_history` opt-in gọi Binance openInterestHist, period mapping như long/short. Samples contracts/value/time, age và % thay đổi đầu-cuối; thiếu hai mẫu/zero baseline trả null. Reject non-finite/bool/âm/missing hoặc timestamp không tăng; không suy diễn trend từ snapshot, không tạo tín hiệu trade/SMC.
- Default vẫn năm sections cũ; manifest tăng output_limit 256 KiB, kiểm tra fixture 400 candles với primary alias. README mô tả schema, giới hạn OI tháng gần nhất và cập nhật cả script/manifest khi cài tool.
- `make check-ai`: **35 PASS, 0 FAIL; 103 regression tests OK**; py_compile và git diff --check pass. Chỉ mock/local tests; chưa Binance/provider thật, GitHub CI hay full WM suite.
- Chưa tự động align/so sánh price-OI, BOS/CHoCH/FVG hoặc phân tích đa khung SMC. OI history chỉ theo primary period, không nhiều period đồng thời; không retry market requests, vẫn cooperative deadline với hạn chế DNS/connect/header cũ.

## Triển khai đợt 14

- Local CLI `history export NAME json|md PATH` và `history delete NAME CONFIRM` dispatch trước provider/key/tools, không cần API key/network. REPL `/export json|md PATH` xuất current RAM, `/delete NAME` yêu cầu gõ lại tên. Private mode chặn cả hai.
- Export giữ metadata/messages/tool payloads, Markdown dùng indented JSON blocks; private 0600, same-directory temp + atomic create-only hardlink publication, không chmod parent/overwrite file/symlink. Không scrub dữ liệu nhạy cảm, không stdout export.
- Delete một snapshot theo stem validated dưới advisory history lock; từ chối symlink/hardlink/non-regular hoặc object thiếu messages list. Session reads cũng bounded nofollow/regular-singlelink. Không xóa alias cùng ID/last/readline/exports/workspace; active writers có thể recreate checkpoint. Không secure erase/bulk retention/undo-delete.
- Test CLI keyless dispatch/roundtrip JSON/Markdown/permissions/no overwrite/private/explicit confirmation, path/symlink/hardlink/malformed guards và REPL export/cancel/delete/private. `make check-ai`: **35 PASS, 0 FAIL; 106 regression tests OK**; py_compile và git diff --check pass. Chưa provider/Binance thật, GitHub CI hoặc full WM suite.
- Lock chỉ serialize cooperating history writers; không chống hostile external writer/race hay hostile destination-directory owner. Markdown là archival view, không model summary hoặc import format. Export/delete không áp dụng workspace tool policy vì là lệnh local trực tiếp của user.

## Triển khai đợt 15

- REPL `/retry` và `/edit-last <text>` chỉ replay latest tool-free user turn, bỏ reply cũ; retry giữ attachments, edit thay toàn bộ prompt/attachments. Từ chối mọi tool call/result, kể cả read-only/pending, không undo/replay tool side effects.
- Failed/cancelled request chưa có tool giữ prompt trong RAM để retry ngay; invalidated bởi prompt mới/slash-command khác, không persist removed prompt. Interrupted tool journal vẫn giữ và retry/edit từ chối.
- Checkpoint original ID rồi publish branch với prompt dự kiến trước khi đổi RAM/call provider; checkpoint lỗi giữ context và không gọi provider. Private mode không backup, chỉ RAM. Usage process-wide không reset, replay có thể bị billing lần nữa.
- Test attachments/replacement/empty/no turn/tool guards, failed request retry, successful edit/retry branches, private, checkpoint failure và interrupted write journal không replay. `make check-ai`: **35 PASS, 0 FAIL; 110 regression tests OK**; py_compile/git diff --check pass. Chưa provider thật/CI/full WM suite.
- Chưa arbitrary turn selection/editor, replay after tool (dù read-only), persistent failed-prompt recovery hoặc stream resume. Checkpoint ID/last vẫn không transaction crash-atomic; nếu publish branch lỗi có thể còn snapshot orphan, RAM giữ nguyên.

## Kết luận

Nền tảng CLI nhẹ và hardening file/process tốt. Ưu tiên hiện tại không phải thêm thật nhiều tool: cần đóng khoảng trống giữa **thao tác thực tế, lịch sử lưu, project hiện tại và quyền được cấp**. Sau đó tối ưu context/search/provider sẽ mang lại lợi ích lớn hơn micro-optimization renderer hay thêm framework nặng.
