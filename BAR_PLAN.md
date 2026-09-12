# BAR_PLAN — bar đẹp hơn (underline + NerdFont + title trái)

Chốt với user (2026-09-12): **WS = Underline**, **Sysmon = Icon+NerdFont**, **Title = Left như cũ**, **Scope = Plan only** (chưa code).
Phạm vi: chỉ `src/bar.c` (+ tối đa 2–3 key config mới), không đổi behavior click/layout/EWMH, không thêm dep ngoài Xft.

## 0. Hiện trạng (audit nhanh)

- Vẽ trong `drawbar()` (`src/bar.c` ~130 dòng): nền phẳng `BAR_BG`, pixmap double-buffer + `XCopyArea`.
- WS: `wsw = S(WS_W)` mỗi ô, active = `FillRectangle` full-height màu accent + label `"*1"`/`" 2"` canh trái `x+S(12)`. Occupied = có `*`, màu `fg`; trống = `dim`. Urgent = chữ màu accent (trùng màu active → khó phân biệt).
- Separator: `XDrawLine` dọc full-height sau khối WS.
- Mode: `"[T] 3n"` + `gaps_on?"":" G-"`, vẽ tại `NWS*wsw+S(10)`.
- Title: `tx = NWS*wsw+S(110)` cố định, không cắt theo vùng trống → dài là đè module phải. Lấy từ `_NET_WM_NAME`/`XFetchName`, đã có `utf8_fit_len` chống cắt dở UTF-8.
- Phải: chuỗi đơn `"C %d%%  M %d%%  B %d  V %s  HH:MM"` một màu `c_sys`, đo `bar_textw(right)` rồi căn phải `barw-rw-S(8)`. CPU/MEM/BAT cache 1s, VOL cache 2s (`sysmon.c`) — giữ nguyên.
- Click (`src/main.c` `ButtonPress`): `Button1` lên bar = `view(e->x / wsw)`, scroll = vol, mid/right = mute. Hit-test WS phụ thuộc `WS_W` — đổi visual không được đổi hit-test.
- Style (`bar_style()`): fontconfig pattern + `scale=` phóng `size=`, fallback `Noto Sans/DejaVu/Sans`, 8 màu (`BAR_*` + 7 override). Reload live qua `Super+Shift+r`.
- Shot `test/shots/02-monocle.png`: bar phẳng, khối active xanh gắt full-height, `*1` thô, mode/title/phải dính nhau, không padding dọc rõ rệt.

## 1. Target (mock ASCII)

Trước:
```
[*1][ 2][ 3][ 4][ 5 ]│[M] 3n  xterm            C 1%  M 13%  V 90%  11:58
```

Sau (bar_h=24, padding đều, underline 2px):
```
  1   2   3   4   5  │  [M] 3n   xterm…          󰻠 1%   󰍛 13%   󰕾 90%   11:58
  ─                  ─────────────────────────────────────────────────────────  ← 1px bottom border dim
  ^^ active: chữ sáng + gạch chân accent 2px (không fill khối)
```

- WS: bỏ `*`, phân biệt bằng màu chữ (active=`ws_active_text`, occ=`ws_occ`, empty=`ws_empty`), active thêm underline accent. Text **căn giữa ô** thay vì `+S(12)`.
- Separator dọc: ngắn lại (từ `y=6` tới `bar_h-6`), màu dim, có padding 2 bên.
- Mode: giữ `[T]/[M] + Nd`, thêm 1 space sau separator; `G-` giữ.
- Title: giữ bên trái sau mode, nhưng `tx = mode_end + pad`, **cắt `…` theo vùng trống** (không đè phải).
- Phải: mỗi module `icon + value`, icon màu accent, value màu `sys`, cách nhau 2 spaces, separator `·` U+00B7 dim (đã chốt, DejaVu có sẵn). Pin yếu/MUTE đổi màu (dùng màu urgent mới).
- Bottom border 1px dim toàn bar (tách bar khỏi cửa sổ).

## 2. Config + state mới (tương thích ngược, thiếu → default cũ)

```ini
font = JetBrainsMono Nerd Font Mono:size=10   # khuyến nghị để có icon; không có vẫn chạy (fallback chữ)
bar_ws_style = underline   # underline (default) | block (block = giữ fill full-height như cũ để rollback)
bar_urgent = e64553        # màu ws urgent + pin yếu + MUTE (default: đỏ Rosé Pine Love)
bar_sep = 6e6a86           # màu separator + bottom border (default = BAR_DIM)
```

- Không thêm `bar_pad*`: padding hardcode theo `S()` (`pad_x=S(10)`, `ws_pad=S(6)`, `underline_h=max(2,S(2))`) để khỏi phình config.
- `bar_ws_active` dùng 2 nghĩa: underline mode = màu gạch chân; block mode = màu fill (giữ code cũ nguyên). `bar_ws_active_text` = màu chữ active.
- Reload live như các key bar hiện tại (`bar_style()` + `drawbar()`).

### 2.1 Checklist state/config (bắt buộc, dễ quên)
- `src/state.h:40`: `barfont_fbs[4]` → `[6]`; `src/state.h:65-66` thêm `C_URGENT, C_SEP` + `h_urgent, h_sep`, `BAR_WS_STYLE` (0=underline, 1=block).
- `src/state.c`: default `C_URGENT=0xe64553 (h=0)`, `C_SEP=0 (h=0 → rơi về BAR_DIM)`, `BAR_WS_STYLE=0`.
- `src/types.h`: `BarColors` thêm `urgent` (XftColor). `sep` KHÔNG cần XftColor — underline/separator/bottom-border vẽ bằng `bargc` + `unsigned long`.
- `config_defaults()`: reset cả 3 key mới (như 7 màu cũ).
- `parse_scalar()`: thêm nhánh `bar_ws_style` (`strcasecmp underline|block`, sai → stderr + giữ cũ), `bar_urgent`/`bar_sep` (via `parse_hex`).
- `load_config()` whitelist unknown-key (~dòng 370): thêm cả 3 tên key, không là config mới bị báo `unknown key` và `test/test-config.sh` fail.
- `bar_style()`: `XftColorFree`/`xft_alloc` thêm `barcol.urgent` (rơi về `C_URGENT` default); GC `bargc` giữ nguyên.

## 3. Design chi tiết

### 3.1 WS underline (thay khối fill)
- Vẽ text căn giữa: `tx = x + (wsw - bar_textw(label))/2`, label chỉ `"1".."10"` (bỏ `"*"`/`" "` prefix).
- Active: `bar_text(ws_acttx)` + `XFillRectangle(x+pad, bar_h-uh-1, wsw-2*pad, uh)` màu `ws_act`. `uh = max(2, S(2))`, `pad = S(6)`.
- Occ/empty: chữ `ws_occ`/`ws_empty`, không underline.
- Urgent (non-active): chữ `barcol.urgent`, không underline (để không lẫn active). Active+urgent: active thắng (chữ `ws_acttx` + underline accent) — hợp lý vì `focus()` thường clear urgent rồi, case này hiếm.
- `block` mode: giữ code cũ nguyên (phục vụ rollback/gu cũ).
- Hit-test không đổi (`e->x / wsw`). `WS_W >= 32` chỉ là khuyến nghị docs khi underline, KHÔNG nâng min config 16 → giữ tương thích.

### 3.2 Icon + fallback (quan trọng nhất)
| module | icon (Nerd Font) | fallback ASCII |
|---|---|---|
| CPU | `󰻠` U+F06E0 | `C` |
| MEM | `󰍛` U+F035B | `M` |
| BAT | `󰁹` U+F0079 (có thể chia 4 mức sau) | `B` |
| VOL | `󰕾` U+F057E / MUTE `󰖁` U+F0581 | `V` / `MUTE` |
| Clock | không icon | `HH:MM` |

- `bar_style()`: KHÔNG dùng 1 string comma. Mở 2 font riêng sau font chính, trước `Noto/DejaVu/Sans`: `XftFontOpenName("JetBrainsMono Nerd Font Mono:size=<scaled>")` rồi `XftFontOpenName("Symbols Nerd Font Mono:size=<scaled>")` (scale `size=` theo `ui_scale` như font chính). Cần nới `barfont_fbs` lên 6 slot (§2.1) mới đủ chỗ.
- Verify codepoint trước khi code: `fc-list | grep -i nerd`, render thử 5 icon trong bảng; glyph nào thiếu thì đổi codepoint khác cùng nghĩa rồi mới chốt vào code.
- Helper mới (static, trong `bar.c`): `const char *pick_icon(const char *icon_utf8, const char *ascii)` — decode utf8 → `FcChar32`, `XftCharExists` trên `barfont` + `barfont_fbs`; có glyph → icon, không → ascii. Tránh tofu `□` trên máy không cài Nerd Font.
- Màu: icon = `BAR_ACC`, value = `c_sys`. MUTE / pin<20% + discharging = `barcol.urgent`.
- Chuỗi phải chuyển từ 1 `snprintf` đơn sang helper `draw_right_segments()`: pass 1 đo từng segment bằng `bar_textw` (từ phải sang: clock trước, CPU cuối) ra `total_w`; pass 2 vẽ trái→sang tại `x = barw - total_w - S(8)` với màu riêng từng segment. Separator `·` U+00B7 màu `C_SEP` (đo/vẽ riêng). Đo/vẽ cùng path `bar_runs` nên alignment exact.

### 3.3 Title trái (fix đè chữ)
- Mode vẽ tại `mx = NWS*wsw+S(10)` nên `mode_end = mx + bar_textw(mode)`; `tx = mode_end + S(12)`.
- Vẽ phải trước (để biết `right_w`), rồi `avail = (barw - right_w - S(8)) - tx - S(8)`; nếu `avail <= 0` → không vẽ title.
- Cắt theo pixel (vì `utf8_fit_len` chỉ cắt theo bytes): `ellipsis_w = bar_textw("…")`; lặp: copy `m = utf8_fit_len(title, len, cap)` vào buf, đo `bar_textw(buf)`; trong khi `> avail - ellipsis_w` thì bớt 1 char UTF-8 cuối rồi đo lại (title ≤112B nên O(n²) vẫn rẻ); append `…`. Không đụng `get_title()` (vẫn lấy full rồi mới cắt lúc vẽ). `…` U+2026 đã có fallback VN.
- Màu giữ `c_title`. Không vẽ nền riêng.

### 3.4 Chrome
- Separator dọc sau WS: `x = NWS*wsw + S(4)`, từ `y=6` tới `bar_h-7`, màu `C_SEP` (rơi về `BAR_DIM`) (thay vì full-height `c_ws_emp`).
- Bottom border: `XFillRectangle(0, bar_h-1, barw, 1)` màu `C_SEP` (nhạt, không giành focus với underline vì underline `[bar_h-uh-1, bar_h-1)` nằm ngay trên nó, kề nhau không đè).
- Baseline giữ công thức `(bar_h + ascent - descent)/2`. Lưu ý `bar_h=8` min: test riêng, underline có thể sát chữ → chấp nhận, khuyến nghị `bar_h>=20` khi underline.

## 4. Phase triển khai (khi duyệt code)

- **P1 — refactor nhẹ `drawbar()` (không đổi visual):** tách `draw_ws()`, `draw_mode_title()`, `draw_right()` static; build + `make check` pass. Rủi ro thấp, để diff P2 gọn.
- **P2 — WS underline + center + bỏ `*`:** default `underline` (đã chốt). `grep test/*.sh` xác nhận chỉ assert geometry cửa sổ, không assert pixel/label bar → không vỡ `make check`.
- **P3 — Title cắt `…` động:** thêm hàm `draw_truncated()`. Test title dài (xterm title 200 ký tự) + title VN.
- **P4 — Sysmon icon + 2 màu + urgent:** fallback chain Nerd Font (2 open riêng, slot 6), `pick_icon()`, `draw_right_segments()`. Test 4 trường hợp: đủ Nerd Font / không Nerd Font (phải ra `C/M/B/V`) / MUTE / pin thấp.
- **P5 — separators + bottom border + docs:** chỉnh separator ngắn, bottom border, cập nhật `README.md` (bảng bar + config mẫu), `config` mẫu trong repo, screenshot mới vào `test/shots/`.
- Mỗi phase: `make clean && make` 0 warning, `make check` ALL PASS, smoke Xephyr 1280x800 + reload `Super+Shift+r` không mất bar.

## 5. Test plan
- `make check` (verify/config/kill/mouse/workspaces/strut/randr) phải ALL PASS sau mỗi phase — đã xác nhận không test nào assert bar visual nên an toàn.
- Thêm case thủ công (không cần tự động ngay): bar_h 16/24/32, scale 1.0/1.5, font `fixed` (không Nerd → fallback chữ), font Nerd (ra icon), ws 1/7/10, title dài/VN, urgent (`:URGENCY` via `xdotool`), MUTE, pin thấp.
- Chụp `test/shots/` trước/sau để so visual.

## 6. Non-goals / rủi ro
- Không transparency, blur, rounded corner, tray, tooltip (cần Composite/XShape → out of scope).
- Icon tofu nếu user không cài Nerd Font → đã có `pick_icon()` fallback, mặc định vẫn đẹp. Codepoint phải verify bằng `fc-list` trước (mục 3.2).
- `bar_textw` đo theo run Gospel — segments 2 màu phải đo/vẽ cùng path `bar_runs` (đã share) nên alignment exact.
- Perf: `drawbar()` 1s/lần + mỗi focus/map; thêm vài `XftTextExtents` không đáng kể. Không gọi `popen` trong draw (giữ cache `sysmon` như cũ).

## 7. Acceptance
- [x] Active WS là underline 2px + chữ giữa ô, không còn khối fill (khi `bar_ws_style=underline`).
- [x] Không còn `*`, không đè chữ title↔sysmon ở mọi `barw >= 800` (title cắt `…` theo pixel).
- [x] Có Nerd Font → icon; không có → tự rớt về `C/M/B/V`, không tofu (`pick_icon`, đã shot cả 2 case).
- [x] MUTE/pin yếu màu `bar_urgent`; reload config live (đã test block→underline + reload); `make check` pass; README + config mẫu cập nhật.

## 8. Thực thi (2026-09-12)
- `src/types.h`: `BarColors` += `urgent`, `sep`.
- `src/state.h/c`: `barfont_fbs[6]`, `C_URGENT` (default `e64553`), `C_SEP` (default → `BAR_DIM`), `h_urgent/h_sep`, `BAR_WS_STYLE=0`.
- `src/config.c`: defaults + `parse_scalar` (`bar_ws_style/urgent/sep`) + whitelist unknown-key.
- `src/bar.c`: WS underline căn giữa (active thắng urgent; active-text fallback `ws_occ` khi user không override để không tàng hình trên nền dark), `pick_icon()`, right segments 2 màu + `·`, title cắt `…`, separator ngắn + bottom border, Nerd fallback chain (size theo font chính × `scale`).
- Fix sau shot: active label default `BAR_BG` tàng hình ở underline mode → dùng `ws_occ` khi `!h_ws_act_tx` (block mode giữ nguyên).
- `make` 0 warning; `make check` ALL PASS; shot Xvfb: Nerd icons + fallback ASCII đều ok.
