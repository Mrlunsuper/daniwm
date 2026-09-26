# TASK-H2-DAMAGE-RECT-20260922.md — dani-comp: repaint theo từng rect damage (giảm CPU khi cuộn)

Trạng thái: **ĐÃ REVERT (2026-09-22)** — M1+M2 gây đen icon tray, đã gỡ khỏi
`src/comp.c` (về clip bbox). Chi tiết xem "Kết quả điều tra" cuối file.

## Kết quả triển khai (2026-09-22)

- M1: `dmg_rects[DmgRectCap=32]` + `dmg_n` + `dmg_overflow`; `dmg_add()` gộp
  song song bbox và append list, tràn cap -> `dmg_overflow` (fallback bbox).
- M2: `back_clip_reset()` dùng list rect (giao với `cur_clip` đã kẹp màn) làm
  clip `back_pict`; `cur_clip` (bbox) vẫn quyết định content-only + occlusion P4.
- M3: **không làm** (giữ update region Present = bbox như cũ) — đúng phương án A.
- Escape hatch: `DANI_COMP_BBOX=1` buộc dùng bbox (dùng để A/B + rollback nhanh).
- Debug metric: `DANI_COMP_DEBUG=1` in `frame nrect=N bbox=Xpx union=Ypx ratio=R`.

### Test đã chạy
- `make clean && make` — 0 warning (`-Wall -Wextra -Wpedantic -Wshadow`).
- `test/test-comp.sh` PASS, `test/test-click-delivery.sh` PASS.
- `test/run-all.sh` — **RESULT: ALL PASS** (29 suite).

### Đo thực tế (test/measure-h2.sh)
- **Cuộn (xterm `seq` liên tục): union/bbox = 0.999** → XDamage báo rect
  **full-window** (1228×748), không phải dải mỏng. Clip list **không giúp gì**
  cho tải cuộn; %CPU ~1.0% cả 2 chế độ (trong nhiễu).
- **Gõ nhiều rect nhỏ rời (typing): union/bbox ≈ 0.66–0.89** → clip list bỏ
  được ~11–34% diện tích vẽ. %CPU chênh trong nhiễu (±0.1%) do compositor
  chủ yếu XFlush-bound dưới Xvfb (server làm phần nặng).
- Kết luận: M1+M2 **an toàn, đúng đắn, có lợi nhẹ** cho frame gộp nhiều rect
  nhỏ rời; **không đạt** mục tiêu "giảm CPU khi cuộn" vì giả định damage
  dạng dải mỏng khi cuộn là **sai** với XDamage thực tế.

---

# TASK-H2-DAMAGE-RECT-20260922.md — dani-comp: repaint theo từng rect damage (giảm CPU khi cuộn)

## Vấn đề

Cuộn trang/terminal/video = **damage mỗi frame dàn đều theo chiều dọc cửa sổ**
(caret, text di chuyển, khối video). Code hiện tại (src/comp.c):

1. `XDamageSubtract` → `XFixesFetchRegion` lấy **danh sách rect** damage
   (src/comp.c ~1300, biến `rr[]`, `nr`) — nhưng rồi **gộp toàn bộ vào 1 hộp
   bao bbox** qua `dmg_add()` (chỉ giữ `dmg_x1..dmg_y2`, `dmg_have`).
   `rg "void dmg_add" src/comp.c` = dòng 152 nhận 1 rect, nội suy bbox).
2. `repaint()` content-only: `cur_clip` = **1 XRectangle** (chính là bbox),
   `XRenderSetPictureClipRectangles(dpy, back_pict, 0,0, &cur_clip, 1)`
   (dòng ~150, n=1), present/present update region cũng là 1 rect.

⇒ Khi cuộn, bbox ≈ **cả cửa sổ** dù phần thực đổi chỉ là vài dải mỏng → mỗi
frame `XRenderComposite` lại toàn footprint + bóng + dim + blit full hộp.
CPU dani-comp tăng đúng theo mật độ cuộn. Đây là "H2: damage theo rect".

## Mục tiêu (đo được)

- Giảm %CPU dani-comp khi cuộn ổn định (không phải lúc fade/dim đổi).
- Giữ nguyên hành vi nhìn thấy: damage thật vẫn vẽ đủ, không sót chữ khi gõ
  nhanh, không sót nội dung cuộn.
- Không phá occlusion culling P4 (cửa sổ bị che trọn thì khỏi vẽ).

## Giải pháp: damage dạng LIST rect (không gộp bbox)

### Thay đổi 1 — lưu damage thành mảng rect có giới hạn
```c
/* thay vì chỉ bbox, giữ luôn danh sách rect damage (root coords, thể hiện
 * damage area ngữ nghĩa = các dải nhỏ). Giới hạn N_RECT cap: nếu tràn thì
 * fallback gộp bbox (dùng thuật toán hiện tại) để không phình vô hạn. */
#define DmgRectCap 32
static XRectangle dmg_rects[DmgRectCap];
static int dmg_n = 0;      /* 0 = không có damage / đã gom */
static int dmg_overflow = 0; /* 1 = tràn cap -> dùng bbox */
```
`dmg_add()` (hiện nhận 1 rect) đổi thành `dmg_add_r(XRectangle r)`:
- nếu `dmg_n < DmgRectCap` → append, gộp luôn bbox song song.
- nếu = cap → `dmg_overflow = 1` (và tiếp tục update bbox).
Khi `dmg_have` được "bao" lại, nếu `dmg_n == 0` (chỉ có bbox từ pre-pass/Expose)
thì xem như 1 rect = bbox (giữ full correct như cũ).

### Thay đổi 2 — repaint dùng cả list rect làm clip
Trong `repaint()` content-only, thay
```c
XRenderSetPictureClipRectangles(dpy, back_pict, 0, 0, &cur_clip, 1);
```
thành
```c
int nc = dmg_overflow ? 1 : dmg_n;
XRectangle *cs = dmg_overflow ? &bbox_as_rect : dmg_rects;
XRenderSetPictureClipRectangles(dpy, back_pict, 0, 0, cs, nc);
```
`cur_clip` giữ = bbox **chỉ để** quyết định content-only vs full + occlusion
(bbox vẫn chính xác cho việc đó). Đừng đổi dataset của `cur_clip`.

### Thay đổi 3 — present update region = list rect
Hiện present/present output dùng 1 rect (`upd_r = cur_clip`). Giữ nguyên **1
region** cho Present cũng an toàn (server tự cắt). Nhưng để tận dụng, có thể
xây `XFixesRegion` từ `dmg_rects[]` rồi truyền region — tuy nhiên **không bắt
buộc**: XPresent với update region dạng 1 bbox đã giúp giảm blit đáng kể. Giữ
phương án hiện tại (1 region bbox) để tối giản rủi ro; chỉ đổi phía CLIP vẽ
(damage thật) là phần đáng giá nhất.

### Thay đổi 4 — thứ tự: vẫn clobber/occlusion như cũ
Occlusion P4 (cửa sổ đục che trọn bbox → `start` chỉ vẽ từ occluder) giữ
nguyên, vì nó dựa trên bbox + `win_is_opaque()`, không dính rect list.

## Rủi ro & guard

- **Bỏ sót damage**: nếu gộp nhầm → sót chữ/khối khi cuộn. Chống: khi
  `dmg_overflow` → dùng bbox (chính xác cũ). `full_dirty` vẫn có, damage
  damage-subtract vẫn báo `ReportNonEmpty` → server đợi subtract nên không
  nuốt damage. Giữ `dirty = 1` cho tới khi repaint tiêu thụ.
- **Trailing gỗ**: `XRectangle dmg_rects[32] * sz` trong stack mỗi frame —
  đừng để stack 32 rect; nếu dùng local, `static` đã có từ clipbuf. Tốt nhất
  dùng `static XRectangle dmg_rect_store[DmgRectCap]` (đã có tiền lệ clipbuf).
- **Present + list clip lệch**: update region server bằng bbox, clip vẽ bằng
  list — nếu server trình update full thì clip vẽ list vẫn đúng vì back_buf
  đã đúng nội dung bên ngoài list (reuse double buffer). OK.
- **Shape window**: hình bounding-shape trong list? Damage shape-rect của
  shaped window vẫn đi qua `dmg_add` path (chỉ convert to root coords) — giữ
  nguyên; đừng clip shaped bằng list (shaped đã có clip riêng P6).

## Kế hoạch test (test/run-all.sh + test-comp.sh)

1. Build sạch (make clean && make) — không warning (dựa Makefile có
   `-Wall -Wextra -Wpedantic`).
2. `test/test-comp.sh` PASS (shadow/fade/opacity/shaped) — occlusion + damage
   không đổi hành vi thấy được.
3. `test-click-delivery` PASS (click vẫn tới app khi có compositor).
4. Đo CPU: chạy `top -b -n1` khi cuộn liên tục 1 terminal/video → ghi %CPU
   trước/sau (mục tiêu giảm rõ).
5. Regression toàn suite: `test/run-all.sh`.

## Quyết định mở — cần owner chốt trước khi code

- **A. Chỉ M1+M2 (clip list rect)**: an toàn nhất, giảm CPU vẽ. *Mặc định.*
- **B. Thêm M3 (present region list)**: thêm hiệu năng present nhưng rủi ro
  Xvfb/public X route omit region. Chỉ bật nếu đo A chưa đủ.

Owner: `/home/dani/daniwm/src/comp.c` + TODO H2. Trạng thái hiện tại: plan.

---

## Kết quả điều tra tray đen (2026-09-22, revert M1+M2)

- Triệu chứng: bật `dani-comp` là icon tray thành hộp đen (tắt comp thì hiện).
  Tái hiện 100% trên Xvfb với `test/tray-icon-helper` (icon đỏ 24x24).
- Chuỗi nguyên nhân (đã đo, không đoán):
  1. Server không báo damage của cửa sổ con (icon) lên damage của cha (bar).
     Damage cha luôn chừa đúng vùng icon (`0,5 1254x14` + `1268,5 12x14`,
     giữa hở 14px = icon). Đây là hành vi XDamage, không phải bug của mình.
  2. H2 clip composite theo list rect (hở vùng icon) nhưng copy nền và
     present vẫn dùng bbox (bao cả vùng icon) -> back_buf và màn hình bị tô
     đen đúng vùng icon, vĩnh viễn. Full-frame vẽ đúng (`backbuf red=196`)
     nhưng tick drawbar 1s sau tô đen lại ngay.
  3. `DANI_COMP_BBOX=1` (clip bbox) hiện icon đỏ ngay -> chốt revert.
- Fix: `src/comp.c` — `back_clip_reset()` luôn clip bbox; `dmg_add()` chỉ giữ
  bbox; gỡ `dmg_rects/dmg_n/dmg_overflow`, metric `frame nrect`, hatch
  `DANI_COMP_BBOX`. Build 0 warning; `test-comp.sh` + `test-tray.sh` +
  `test-click-delivery.sh` PASS; repro Xvfb hiện đỏ.
- Muốn tối ưu clip list sau này thì phải track Damage riêng cho từng icon
  con của bar (SubstructureNotify trên bar + XDamageCreate mỗi icon, dịch tọa
  độ về root rồi mới `dmg_add`), đồng thời copy nền + present cũng phải dùng
  list — không làm nửa vời như M1+M2.
