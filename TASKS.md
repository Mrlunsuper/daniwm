# TASKS — Tách module `daniwm.c`

Nguồn: [`refactor_plan.md`](refactor_plan.md). Kế hoạch xử lý các điểm mơ hồ: [`GAPS_PLAN.md`](GAPS_PLAN.md).  
Phạm vi: **refactor thuần túy** — không đổi behavior, không thêm feature.

## Trạng thái thực thi — HOÀN TẤT (2026-09-12, nhánh `refactor/modules`)

Tất cả phase đã xong qua 8 subagent tuần tự (model `deepseek-v4-pro`) + audit độc lập bởi parent.

| Commit | Phase | Kết quả |
|---|---|---|
| `850f999` | 0.5–2 | `src-check`, `types.h`, `state.h/c`, skeleton 10 header — 0 warning |
| `1aed49e` | 3–4 | `sysmon.c`, `monitor.c` (+`screen_extents`) — 0 warning |
| `afb0a98` | 5 | `ewmh.c` — 0 warning, body diff byte-identical |
| `4e58827` | 6–7 | `layout.c`, `client.c` — 0 warning |
| `96c976e` | 8 | `bar.c` — 0 warning |
| `42f8689` | 9–10 | `mouse.c`, `keys.c` — 0 warning |
| `cdc0560` | 11 | `config.c` (+G17 `nactions`) — 0 warning |
| `27871bc` | 12–14 | `main.c`, Makefile thật, xóa `daniwm.c` — link OK |

**Verification cuối (parent, độc lập)**: build 0 warning / 0 error; `make check` ALL PASS;
`.text` 71515 vs baseline 69062 (**+3.55%**); so từng thân hàm: 119/123 giống hệt,
4 diff đúng các fix đã duyệt (G3 `keys_reset` ×3, G17 `nactions` ×1); 0 hàm nguồn bị mất;
`nm` không trùng symbol; boot smoke Xephyr OK (`_NET_SUPPORTING_WM_CHECK` set).

**Deviations đã duyệt**: `keys`/`nkeys`/`k_vol_*` export từ `keys.c` (main.c dùng trực tiếp,
khớp baseline); `struct MonitorGeom` đặt tên cho struct `mons` ẩn danh; header comment
gốc của `daniwm.c` được rút gọn thành header mới ở `src/main.c`.

## Quyết định đã chốt

- [x] Bố cục: tất cả `.c/.h` mới nằm trong **`src/`**, Makefile build đa object.
- [x] Task tracking: file này (`TASKS.md`), không dùng runtime mission.
- [x] Rollback: dùng **git** (`git checkout`/`git stash`), **không** tạo `daniwm.c.bak`.
- [x] Binary output vẫn ở **repo root** (`./daniwm`), vì `test/*.sh` gọi `$TDIR/../daniwm`.
- [x] Không đổi tên hàm/struct/typedef.
- [x] `daniwm.c` gốc bị xóa ở bước cuối, không giữ song song (tránh 2 nguồn sự thật).

## Bất biến (invariants) — kiểm tra sau MỖI bước

- [x] `make clean && make` → **0 warning** (baseline hiện tại cũng 0).
- [x] `make check` → `RESULT: ALL PASS`.
- [x] Binary size trong **±5%** so với baseline.
- [x] Không có thay đổi logic: chỉ `static` bị gỡ, code di chuyển nguyên khối.

---

## Phase 0 — Baseline & an toàn

- [x] **T0.1** Chốt worktree sạch: `git status` chỉ có `refactor_plan.md` + `TASKS.md` untracked.
- [x] **T0.2** Build baseline và lưu hash:
  ```bash
  make clean && make && cp daniwm /tmp/daniwm.baseline
  sha256sum /tmp/daniwm.baseline && size daniwm
  ```
- [x] **T0.3** Chạy baseline test, lưu log: `make check 2>&1 | tee /tmp/check.baseline.log`.
- [x] **T0.4** Commit `refactor_plan.md` + `TASKS.md` (chốt điểm rollback trước khi tách).
- [x] **T0.5** Tạo nhánh: `git switch -c refactor/modules`.
- [x] **T0.6** (G0) Thêm target TEMP `src-check` vào Makefile (compile mọi `src/*.c`, không link) + commit riêng — xem `GAPS_PLAN.md` §2.

---

## Phase 1 — `src/types.h` (không code thực thi)

- [x] **T1.1** Tạo `src/types.h`, include các X11 header cần cho type (`Xlib.h`, `Xft.h`) + `#pragma once`.
- [x] **T1.2** Di chuyển nguyên văn:
  - `Rule` — dòng 28
  - `Client` / `struct Client` — 61–73
  - `Layout` enum — 75
  - `BarColors` — 86
  - `StrutMargin` — 114
  - `Dock` / `struct Dock` — 117–123
  - `Drag` — 1514
  - `Key` — 1641
- [x] **T1.3** `make` vẫn build từ `daniwm.c` (chưa include types.h) → phải pass, chứng minh header không phá gì.
- [x] **T1.4** (G0) Tạo **skeleton toàn bộ `.h`** (`state/monitor/sysmon/bar/ewmh/layout/client/mouse/keys/config`) với `#pragma once` + prototype cuối cùng theo ownership matrix trong `GAPS_PLAN.md` §3. Chốt API trước khi đổ impl.

## Phase 2 — `src/state.h` + `src/state.c`

- [x] **T2.1** `state.h`: `#pragma once`, `#include "types.h"`, khai báo `extern` **toàn bộ** global:
  - X11/display: `dpy`, `root`, `bar`, `checkwin`, `screen`, `sw`, `sh` (dòng 77–80, 93)
  - clients: `clients`, `sel`, `ws_sel`, `curws`, `nws_alloc` (94–100)
  - layout arrays: `ws_layout`, `ws_mfact`, `ws_nmaster` (98–100)
  - monitors: `mons` (struct ẩn danh), `nmons`, `mon_struts`, `rr_event_base/error_base/present` (103–106, 115)
  - docks/bar: `docks`, `barw`, `barpm`, `bargc`, `barfont`, `barfont_fbs`, `bar_nfb`, `barxd`, `barcol`, `barcol_ok` (79–88, 124–125)
  - sysmon cache: `cpu_prev_total`, `cpu_prev_idle`, `vol_cache`, `vol_ts` (90–92)
  - config: `rules`, `nrules`, `caprules`, `termcmd`, `menucmd`, `scratchcmd`, `MOD`, `BORDER`, `BORDER_FOCUS/NORMAL`, `BAR_*`, `BAR_H`, `WS_W`, `ui_scale`, `font_name`, `bar_on`, `gaps_on`, `gap_outer`, `gap_inner`, `def_mfact`, `def_nmaster`, `NWS` (28–58)
  - EWMH atoms: `A_NET_*` (127–135)
  - cursors: `cur_move`, `cur_resize`, `cur_hsplit` (1516)
- [x] **T2.2** `state.h`: đặt `#define MAXWS 10` (57), `MAXMONS 16` (102), `FLOAT_STEP 20`, `RSZ_STEP 20` (1689–1690).
- [x] **T2.3** `state.h`: đặt 3 macro layout (dòng 153–155):
  ```c
  #define LAYOUT  (ws_layout[curws])
  #define MFACT   (ws_mfact[curws])
  #define NMASTER (ws_nmaster[curws])
  ```
- [x] **T2.4** `state.h`: chuyển `S()` thành `static inline` (dòng 49) — dùng bởi layout/mouse/keys/bar; phụ thuộc `ui_scale` (đã `extern` ở T2.1).
- [x] **T2.5** `state.c`: `#include "state.h"` + định nghĩa mọi biến với **đúng giá trị khởi tạo** như dòng 27–165 (giữ `= NULL`, `= 0`, `= 1.0f`, `= 5`, …).
- [x] **T2.6** Kiểm tra: `nm -g state.o` liệt kê đủ symbol; `daniwm.c` chưa đổi.

> ⚠️ `S()` ở `state.h` phải là `static inline` (không phải prototype) — nếu chỉ khai báo, linker sẽ thiếu symbol ở mọi module.

## Phase 3 — `src/sysmon.c/h` (độc lập nhất, không cần X11)

- [x] **T3.1** Tạo `sysmon.h`: `sys_cpu`, `sys_mem`, `sys_bat`, `sys_vol`, `sys_vol_update`, `vol_set_cmd` (dùng `size_t` → include `<stddef.h>`).
- [x] **T3.2** Tạo `sysmon.c`, chuyển dòng **431–560** nguyên khối: `sys_cpu` 432, `sys_mem` 446, `sys_bat` 462, `sys_vol` 488, `vol_try_amixer` 491, `vol_try_wpctl` 509, `vol_try_pactl` 522, `sys_vol_update` 542, `vol_set_cmd` 558.
- [x] **T3.3** Giữ `static` cho `vol_try_*`; gỡ `static` cho 6 hàm public. `vol_up_*`/`vol_down_*`/`vol_mute_*` argv vẫn thuộc `keys.c` (Phase 10) — không chuyển sang đây.
- [x] **T3.4** `make` pass với `daniwm.c` cũ (file mới chưa link) → chỉ cần compile sạch.

## Phase 4 — `src/monitor.c/h`

- [x] **T4.1** `monitor.h`: `initmons`, `mon_at`, `mon_by_pointer`, `getarea`, `on_monitors_changed`, `screen_extents`.
- [x] **T4.2** `monitor.c` ← dòng **210–294**: `initmons` 211, `mon_at` 239, `mon_by_pointer` 245, `getarea` 255, `on_monitors_changed` 271.
- [x] **T4.3** `screen_extents` (1220–1233) đang nằm trong vùng EWMH → **quyết định**: giữ ở `monitor.c` (đúng ngữ nghĩa hình học màn hình) và khai báo trong `monitor.h`. Ghi chú lại deviation so với plan.
- [x] **T4.4** Gỡ `static` khỏi các hàm public; `getarea` dùng `S`, `BAR_H`, `gaps_on`, `gap_outer`, `bar_on` → include `state.h`.

## Phase 5 — `src/ewmh.c/h`

- [x] **T5.1** `ewmh.h`: 19 hàm theo plan + `find_dock`, `ewmh_read_desktop` (cross-module). `get_strut` giữ `static` trong `ewmh.c` — không export.
- [x] **T5.2** `ewmh.c` ← dòng **992–1387**: `ewmh_init` 993, `ewmh_hasstate` 1040, `ewmh_update_state` 1051, `set_urgent` 1064, `ws_has_urgent` 1070, `ewmh_isfloating_type` 1075, `ewmh_client_list` 1087, `ewmh_active` 1100, `ewmh_set_wm_desktop` 1109, `ewmh_desktops` 1116, `ewmh_read_desktop` 1137, `setfullscreen` 1152, `find_dock` 1174, `get_strut` 1180, `ewmh_isdock` 1234, `update_struts` 1250, `manage_dock` 1351, `unmanage_dock` 1369, `update_dock_strut` 1381.
- [x] **T5.3** `screen_extents` **không** nằm ở đây (đã sang `monitor.c` — T4.3).
- [x] **T5.4** `setfullscreen` gọi `arrange()` → include `layout.h`.

## Phase 6 — `src/layout.c/h`

- [x] **T6.1** `layout.h`: `tile`, `tile_mon`, `monocle`, `monocle_mon`, `arrange`.
- [x] **T6.2** `layout.c` ← dòng **295–429**: `tile_mon` 296, `tile` 361, `monocle_mon` 369, `monocle` 396, `arrange` 399.
- [x] **T6.3** Dùng `S`, `LAYOUT`, `MFACT`, `NMASTER`, `gaps_on`, `mon_struts` → include `state.h` (+ `monitor.h`).

## Phase 7 — `src/client.c/h`

- [x] **T7.1** `client.h`: `find`, `ws_occupied`, `first_in_ws`, `count_tiled`, `attach`, `detach`, `manage`, `unmanage`, `focus`, `focus_step`, `view`, `send_to`, `move_to`, `kill_sel`, `kill_client`, `spawn`, `toggle_floating_sel`, `keep_docks_on_top`, `quit`.
- [x] **T7.2** `client.c` gom từ 4 vùng:
  - helpers **167–209**: `find` 168, `ws_occupied` 174, `first_in_ws` 180, `count_tiled` 186, `attach` 193, `detach` 200
  - actions **803–990**: `keep_docks_on_top` 803, `focus` 808, `focus_step` 828, `view` 849, `send_to` 867, `move_to` 887, `kill_client` 920, `kill_sel` 944, `spawn` 957, `toggle_floating_sel` 971, `quit` 990
  - **matchrules 1389–1406** ← plan bỏ sót (G1), giữ `static`
  - **last_kill_win/last_kill_time 942–943** (G10) — giữ `static`, gộp vào đây
  - manage/unmanage **1450–1537**: `manage` 1450, `unmanage` 1518
- [x] **T7.3** `findscratch` 1408–1418 **không** để ở đây → sang `keys.c` (chỉ `k_scratch` dùng).
- [x] **T7.4** `client.c` cần `layout.h` (`arrange`), `ewmh.h` (`ewmh_*`, `setfullscreen`, `update_struts`), `bar.h` (`drawbar`), `mouse.h` (`grabbuttons`, `drag_start`).

## Phase 8 — `src/bar.c/h`

- [x] **T8.1** `bar.h` **chỉ export** `bar_style` + `drawbar` (G6/siết lại so với plan: `bar_text`, `bar_textw`, `bar_runs`, `utf8_*`, `get_title`, `xft_alloc`, `bar_glyph_font`, `scaled_font_pat` **giữ `static`** vì không có caller ngoài `bar.c`).
- [x] **T8.2** `bar.c` ← dòng **561–802**: `xft_alloc` 562, `bar_text` 575, `bar_textw` 579, `bar_glyph_font` 584, `utf8_decode` 592, `bar_runs` 611, `utf8_fit_len` 640, `get_title` 659, `drawbar` 691.
- [x] **T8.3** `bar_style` **2267–2317** và `scaled_font_pat` **2246–2265** nằm tận cuối file → chuyển về `bar.c` (plan ghi `bar.c` dòng 561–798 nhưng thực tế `bar_style` ở 2267).
- [x] **T8.4** `bar.c` include `sysmon.h` cho `sys_cpu/sys_mem/sys_bat/sys_vol`, `client.h` cho `ws_occupied`.
- [x] **T8.5** `bar_style` gỡ `static`, khai báo ở `bar.h` (dùng bởi `main` và `bar.c`).

## Phase 9 — `src/mouse.c/h`

- [x] **T9.1** `mouse.h`: `grabbuttons`, `drag_start`, `drag_motion`, `drag_end`.
- [x] **T9.2** `mouse.c` ← dòng **1539–1638**: `grabbuttons` 1539, `drag_start` 1547, `drag_motion` 1570, `drag_end` 1615.
- [x] **T9.3** `Drag` struct → `types.h` (T1.2). Biến `Drag drag = { 0 }` (dòng 1515) và `cur_move/cur_resize/cur_hsplit` (1516) → `extern` trong `state.h` (T2.1), định nghĩa trong `state.c`.
- [x] **T9.4** `mouse.c` include `client.h` (focus/attach), `layout.h` (arrange), `bar.h` (drawbar nếu cần).

## Phase 10 — `src/keys.c/h`

- [x] **T10.1** `keys.h`: `grabkeys`, `add_default_keys`, `keys_reset`, `push_key_fn`, `k_reload`. (`keys`/`nkeys`/`capkeys` là `static` — không export.)
- [x] **T10.2** `keys.c` ← dòng **1641–1743**: `Key *keys` 1642, `nkeys/capkeys` 1643, toàn bộ `k_*` 1648–1743, `actions[]` 1728–1743, `FLOAT_STEP`/`RSZ_STEP` 1689–1690.
- [x] **T10.3** `grabkeys` **2231–2244** → `keys.c`.
- [x] **T10.4** `findscratch` **1408–1418** (G2, giữ `static`) + `k_scratch` **1420–1448** → `keys.c`.
- [x] **T10.5** **Ownership của key table** (G3+G11): `keys`/`nkeys`/`capkeys` **`static` trong `keys.c`** (không đưa vào `state.h`). Chuyển `push_key_fn` (1766–1774) và `add_default_keys` (1831–1872) từ `config.c` sang `keys.c`. `config.c` reset qua API `void keys_reset(void);` thay vì gán `nkeys = 0` trực tiếp (dòng 1876, 2155, 2169).
- [x] **T10.6** `actions[]` có `k_reload` (2208–2229) — impl ở `config.c`. Khai báo `k_reload` trong `keys.h`, **không** include `config.h` từ `keys.h` (G15).
- [x] **T10.7** argv âm lượng `vol_up_am/wp/pa`, `vol_down_*`, `vol_mute_*` (1675–1686) → `keys.c`.

## Phase 11 — `src/config.c/h`

- [x] **T11.1** `config.h`: `load_config`, `config_defaults`, `finalize_nws`, `k_reload` (theo T10.6).
- [x] **T11.2** `config.c` ← dòng **1745–2230**: `xstrdup` 1745, `trim` 1752, `strip_comment` 1760, `push_key_fn` 1766 (**chuyển sang keys.c** — T10.5), `push_rule` 1775, `free_argv` 1785, `split_argv` 1790, `set_cmd` 1803, `parse_bool` 1809, `parse_hex` 1814, `mod_from_name` 1825, `add_default_keys` 1831, `config_defaults` 1873, `config_path` 1895, `parse_scalar` 1903, `parse_rule` 1968, `parse_bind` 2002, `load_config` 2080, `finalize_nws` 2177, `k_reload` 2208.
- [x] **T11.3** `config.c` include `keys.h` (T10.5/T10.6), `client.h` (`view`, `move_to` khi reload?), `ewmh.h` (`ewmh_desktops`), `bar.h` (`bar_style`), `layout.h` (`arrange`).
- [x] **T11.4** ⚠️ Không nhầm file config runtime ở repo root tên **`config`** với `src/config.c`. `config_path()` đọc `~/.config/daniwm/config` — giữ nguyên.

## Phase 12 — `src/main.c` + xerror

- [x] **T12.1** `main.c` ← dòng **2319–2325** (`xerror_other_wm`, `xerror_ignore`) + **2327–2619** (`main`).
- [x] **T12.2** `main.c` include: `types.h`, `state.h`, `monitor.h`, `ewmh.h`, `bar.h`, `client.h`, `layout.h`, `config.h`, `keys.h`, `mouse.h`, `sysmon.h` (nếu cần), `bar.h`.
- [x] **T12.3** `xerror_*` giữ `static` (chỉ `main` dùng).
- [x] **T12.4** `main.c` dùng trực tiếp `keys`/`nkeys` (vòng dispatch KeyPress) và `k_vol_*` (scroll) — đã xác minh khớp baseline → export từ `keys.h`, `capkeys` vẫn `static`.

## Phase 13 — Makefile

- [x] **T13.1** Sửa `Makefile`:
  ```makefile
  SRCS = src/state.c src/monitor.c src/sysmon.c src/bar.c src/ewmh.c \
         src/layout.c src/client.c src/mouse.c src/keys.c src/config.c src/main.c
  OBJS = $(SRCS:.c=.o)
  CFLAGS += -MMD -MP
  daniwm: $(OBJS)
      $(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
  %.o: %.c
      $(CC) $(CFLAGS) -c -o $@ $<
  -include $(OBJS:.o=.d)
  ```
- [x] **T13.2** `clean` xóa `src/*.o`, `src/*.d`.
- [x] **T13.3** (G16) Giữ target `daniwm` xuất ra **root** để `test/*.sh` không phải sửa; object nằm `src/*.o`.
- [x] **T13.4** `test/dock-helper` vẫn build từ `test/dock-helper.c` như cũ.
- [x] **T13.5** (G13) `.gitignore` thêm `src/*.o` và `src/*.d`.

## Phase 14 — Xóa file gốc & verify cuối

- [x] **T14.1** `git rm daniwm.c` (sau khi mọi thứ build từ `src/`).
- [x] **T14.2** `make clean && make` → 0 warning. Kiểm tra thêm `-Wmissing-prototypes -Wmissing-declarations -Wredundant-decls` (G9) và `nm -g` diff (GAPS_PLAN.md §6).
- [x] **T14.3** So sánh size với baseline (`.text`/tổng) — trong ±5%.
- [x] **T14.4** `make check` → `RESULT: ALL PASS`, diff với `/tmp/check.baseline.log`.
- [x] **T14.5** Smoke test Xephyr `1280x720`: tiling, monocle, ws switch, gaps, bar, floating, scratchpad, fullscreen, config reload (`Mod+Shift+R`).
- [x] **T14.6** `git diff --stat` — không có thay đổi logic ngoài di chuyển code + gỡ `static`.
- [x] **T14.7** Commit cuối, xóa nhánh nếu merge xong.

---

## Plan gaps đã phát hiện (chi tiết: [`GAPS_PLAN.md`](GAPS_PLAN.md))

| # | Loại | Vấn đề | Xử lý |
|---|---|---|---|
| G0 | 🔴 nặng | Makefile chỉ trỏ `daniwm.c` → `make` không compile `src/*.c` giữa các phase | target TEMP `src-check` (T0.6) |
| G1 | bỏ sót | `matchrules` (1389–1406) không được plan gán module | → `client.c`, giữ `static` (T7.2) |
| G2 | bỏ sót | `findscratch` (1408–1418) không được plan gán module | → `keys.c`, giữ `static` (T10.4) |
| G3 | mâu thuẫn | `keys/nkeys/capkeys/push_key_fn` vừa ở `keys.h` vừa ở `config.c` | định nghĩa `keys.c` (`static`), API `keys_reset()` + `push_key_fn` ở `keys.h` (T10.5) |
| G4 | thật | `S()` + `ui_scale` dùng chéo gần như mọi module | → `static inline` trong `state.h` (T2.4) |
| G5 | bỏ sót | `LAYOUT/MFACT/NMASTER` macro (153–155) | → `state.h` (T2.3) |
| G6 | ghi sai vị trí | `bar_style` (2267) + `scaled_font_pat` (2246) ở cuối file, plan ghi dải 561–798 | → `bar.c` (T8.3) |
| ~~G7~~ | ✅ không phải gap | `screen_extents` (1220): plan **đã** gán cho `monitor.c` | → `monitor.c`, `ewmh.c` include `monitor.h` (T4.3) |
| G8 | bỏ sót | `MAXWS/MAXMONS/FLOAT_STEP/RSZ_STEP` rải rác | → `state.h` (T2.2) |
| G9 | thật | Mọi hàm là `static` → gỡ ở public, giữ ở helper | ownership matrix `GAPS_PLAN.md` §3 |
| ~~G10~~ | ✅ không phải gap | `last_kill_win/time` (942–943) chỉ `kill_sel` dùng | giữ `static` trong `client.c`, **không** vào `state.h` |
| G11 | mâu thuẫn | `add_default_keys` (1831) phải ở `keys.c` chứ không phải `config.c` | → `keys.c` (T10.5) |
| G12 | plan đúng | `get_strut` (1180) chỉ ewmh dùng | giữ `static`, không export header |
| G13 | vận hành | `.gitignore` thiếu `src/*.o`, `src/*.d` | thêm (T13.2) |
| G14 | rủi ro | `-Wshadow` + include `state.h` khắp nơi → local trùng tên global | rename local, không tắt flag (T14.2) |
| G15 | rủi ro | header cycle `keys.h ↔ config.h` (`k_reload` vs `push_key_fn`) | `k_reload` khai báo ở `keys.h`, impl ở `config.c`; `keys.h` không include `config.h` |
| G16 | vận hành | `test/*.sh` gọi `$TDIR/../daniwm` | binary **phải** ở repo root, object ở `src/*.o` (T13.3) |

## Acceptance criteria

- [x] Build sạch 0 warning với flags hiện tại (`-Wall -Wextra -Wpedantic -Wshadow` + hardening).
- [x] 11 module `.c` + 10 header trong `src/`, `daniwm.c` đã xóa.
- [x] `make check` 100% pass như baseline.
- [x] Không có symbol global trùng định nghĩa (`nm` không thấy duplicate).
- [x] Git history cho thấy refactor từng bước, mỗi commit build được.
