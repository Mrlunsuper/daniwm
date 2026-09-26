# FIX REPORT — Audit WM 2026-09-25 (AUDIT-WM-20260925-233536.md)

Date (UTC): 2026-09-26. Branch `main`, 9 commits mới trên `b22ebb6`.
Toolchain: gcc 14.3.0 (system), Xvfb + Xephyr có sẵn.
`cppcheck / scan-build / clang-tidy / valgrind`: không có trên máy (giống audit).

Lưu ý worktree: đầu session worktree dirty vì feature work chưa commit.
Đã `git stash push -m "WIP pre-audit-fix stash 2026-09-25"` trước khi fix,
fix trên HEAD sạch để mỗi commit chỉ chứa đúng phần fix. Cuối session
`git stash pop` để trả lại worktree (xem mục 4).

## 1. Bảng kết quả

| # | Finding | Verdict | Commit | Test reproduce | Trạng thái |
|---|---|---|---|---|---|
| 1 | `keys.c` uninit `a.map_state` | CONFIRMED | `756a269` fix(scratchpad) | đọc code + build 0 warning + `test-restart.sh` PASS | FIXED |
| 2 | `tray.c` forged REQUEST_DOCK | CONFIRMED | `692d82a` fix(tray) | probe `/tmp/opencode/tray-spoof-test.c`: pre-fix `DOCKED_SPOOF` (parent=bar), post-fix `REJECTED` + log `ignore dock for non-icon`; `test-tray.sh` PASS (icon hợp lệ vẫn dock) | FIXED |
| 3 | `rename.c` shell-injection qua env | FALSE POSITIVE | — | chạy đúng snippet `sh -c` với `XDG_CACHE_HOME='/tmp/a";touch /tmp/pwn;#'`: không có `/tmp/pwn`; `file` đi qua `setenv`, expand an toàn trong `"$VAR"`; `eval` chỉ chạy `RENAME_CMD` là config của user (đã document là shell cmd) | NOT FIXED |
| 4 | `client.c` ghost-client race | CONFIRMED | `30b4b5c` fix(manage) | đọc code + `test-ghost-unmap.sh` PASS (gồm storm 20x map/unmap) | FIXED |
| 5 | `layout.c` arrange storm, không throttle | CONFIRMED, DEFERRED | — | đếm round-trip: mỗi `arrange()` = 1 `XQueryTree` + O(n) request + `XSync` + `drawbar`; fix thật cần coalesce ở event loop (đổi timing semantics, rủi ro regression) → không sửa bừa, ghi hướng làm sau | NOT FIXED |
| 6 | `CurrentTime` khắp focus/kill | CONFIRMED (thu hẹp scope) | `3e3b931` fix(focus) | `test-takefocus.sh`, `test-focus-model.sh`, `test-focus-steal.sh`, `test-kill.sh` PASS; `tray.c` selection/XEMBED, `mouse.c` grab/ungrab giữ `CurrentTime` (đúng spec) | FIXED |
| 7 | `config.c` wordexp glob bomb | CONFIRMED | `a10087c` fix(config) | config `ws_names = /*/*/*/*/*` → WM khởi động bình thường, names fallback `1..5`, không spike; `test-config.sh` PASS | FIXED |
| 8 | EWMH ClientMessage không check sender | BY DESIGN | `923f6e6` docs(ewmh) | đúng spec EWMH trust-based (pager/wmctrl cần); chỉ thêm 1 câu document vào README, không đổi code | DOCUMENTED |
| 9 | `comp.c` leak `XGetAtomName` | CONFIRMED | `65224d9` fix(comp) | đọc code + build + `test-comp.sh` PASS | FIXED |
| 10 | `ws_icon_N` xé UTF-8 | CONFIRMED | `885f7f3` fix(low) | copy boundary-fix từ `ws_set_name`; `test-config.sh` PASS | FIXED |
| 11 | drag `swap_target` rò highlight | CONFIRMED | `885f7f3` fix(low) | đọc code + `test-mouse.sh` PASS | FIXED |
| 12 | `bar.c:774` `-Wsign-conversion` | CONFIRMED | `885f7f3` fix(low) | `gcc -Wconversion`: từ 1 warning → 0 warning | FIXED |
| 13 | `xerr.c` `trap_dpy` dead-store | CONFIRMED MỘT PHẦN | `885f7f3` fix(low) | xóa `trap_dpy`; nesting đã đúng via `trap_depth` (audit mô tả sai phần này); `test-xerr.sh` PASS | FIXED |
| 14 | advertise MODAL/ABOVE nhưng handler chỉ FS/DA | CONFIRMED | `885f7f3` fix(low) | gỡ 2 atom khỏi `sup[]` + sửa comment; property-level honoring giữ nguyên; `test-supported.sh` PASS | FIXED |
| 15 | thiếu `XSetIOErrorHandler` | CONFIRMED | `ed2ab98` fix(main) | kill Xvfb khi WM chạy → log `daniwm: X I/O error, exiting` (trước đây exit câm) | FIXED |
| 16 | `grabbuttons` bỏ Button2, xi2 không fallback | FALSE POSITIVE | — | bỏ Button2 là cố ý (middle-click thuộc về app); xi2 fail đã có log; fallback `ReplayPointer` gây loop trên Xorg như audit tự ghi | NOT FIXED |
| 17 | `bar.c` 4 biến chết | CONFIRMED MỘT PHẦN | `885f7f3` fix(low) | chỉ `c_task_act` + `c_sys` chết thật (`c_mode`/`c_title` vẫn dùng ở draw); xóa đúng 2 biến | FIXED |
| 18 | autostart TOCTOU `access→execl` | CONFIRMED | `885f7f3` fix(low) | `stat()` chọn file + cảnh báo world-writable + log khi `execl` fail | FIXED |
| 19 | `monitor.c` không shrink float | CONFIRMED | `885f7f3` fix(low) | shrink `fw/fh` trước clamp pos (guard monitor 0) | FIXED |
| 20 | focus/stack/ws logic | INFO, không bug | — | audit tự kết luận giữ nguyên | — |

## 2. Tổng kết

- FIXED code: #1, #2, #4, #6, #7, #9, #10, #11, #12, #13, #14, #15, #17, #18, #19 (15 findings).
- DOCUMENTED (by design): #8.
- FALSE POSITIVE: #3 (chứng minh thực nghiệm), #16 (cố ý).
- DEFERRED: #5 (cần coalesce event-loop, rủi ro > lợi ích khi sửa mù).
- INFO: #20.

## 3. Verify cuối

- `make daniwm dani-comp dani-run`: 0 warning (`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`).
- `gcc -Wconversion`: 0 warning (audit ghi 1 ở `bar.c:774`).
- `gcc -fanalyzer`: 0 warning (khớp audit).
- `test/run-all.sh`: 29/29 suites PASS (lưu ý: phải build helper qua `make check`
  trước; chạy `run-all.sh` trực tiếp thiếu `test/click-helper`).
- `MALLOC_CHECK_=3`: `test-ws-storm.sh` (50 flips, 10 clients) + `test-ghost-unmap.sh` PASS.
- ASan/UBSan runtime (`libasan.so.8.0.0`/`libubsan`): không có trên máy
  (linker script của gcc hệ thống trỏ file không tồn tại) → không chạy được,
  đã thay bằng `-fanalyzer` + suite Xephyr + probe repro.
- Mỗi commit `git show --stat`: 1–7 files, diff nhỏ, đúng phần fix.
  Ngoại lệ: commit `a10087c` từng build hỏng (comment chứa `/*` tự đóng
  comment) → đã phát hiện, sửa comment, `git commit --amend` (HEAD local),
  rebuild OK + `test-config.sh` PASS.

## 4. Còn lại / giao lại

1. `#5 arrange throttle`: hướng làm sau — dirty-flag + 1 `arrange()` sau khi
   drain event queue, cache 1 `XQueryTree`/arrange, cap `MAXCLIENTS`. Cần
   test 10k-window + full suite lại vì đổi timing semantics.
2. Worktree của bạn (feature work stash lúc đầu) đã được `git stash pop` trả
   lại — có đúng 1 conflict ở `src/main.c` (EnterNotify: dòng `last_evtime`
   của fix #6 cạnh dòng `FOCUS_MODE == 1` của bạn), đã resolve giữ cả 2,
   merged tree build 0 warning. Stash gốc vẫn giữ (`stash@{0}`) phòng hờ —
   bạn kiểm tra `git status`/`git diff --cached` rồi `git stash drop` khi ưng.
3. `FIX_REPORT.md` này đang untracked — commit hay không tùy bạn.
4. Chưa chạy `cppcheck/clang-tidy` (máy không có tool) như audit đã nêu.
