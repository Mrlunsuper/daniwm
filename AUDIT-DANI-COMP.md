# Audit dani-comp

Phạm vi: `src/comp.c`, `Makefile`, `test/test-comp.sh` tại HEAD `d162dd2`.
Không sửa source trong đợt audit này.

## Kiểm chứng

- Rebuild `make all`: thành công, không có compiler warning.
- `test/test-comp.sh`: toàn bộ assertion PASS sau khi build đủ binary.
- Harness C tạm trong `/tmp/dani-comp-audit.c`, include trực tiếp comp.c để kiểm tra state nội bộ, chạy trên Xvfb riêng: xác nhận các lỗi cache, shape, lịch shape và error trap bên dưới.
- Chưa kiểm tra trực tiếp đường flip trên Xorg/GPU thật. Các finding XPresent là phân tích source/protocol, không phải kết quả benchmark phần cứng.
- Không chạy toàn bộ WM regression suite.

## 1. HIGH — stack_nk đang dùng lẫn capacity và số phần tử hợp lệ

**Vị trí:** `src/comp.c:793-811`.

Khi QueryTree trả ít cửa sổ hơn trước, code chỉ copy `nk` phần tử nhưng không cập nhật `stack_nk` (vì chỉ cập nhật khi tăng capacity). Frame content-only tiếp theo lấy `nk = stack_nk`, duyệt cả đuôi cache cũ.

Harness: tạo A/B/C, hủy B rồi repaint stack-dirty:

```
stack initial capacity/count=3
after destroy actual=2 cached-used-next-frame=3
```

Đuôi cũ chứa C, nên C bị vẽ hai lần ở frame sau. Cửa sổ opacity/ARGB hoặc đang fade bị blend hai lần; bóng cũng có thể bị nhân đôi. XID stale có thể gây thêm truy vấn lỗi.

**Sửa:** tách `stack_cap` và `stack_count`; luôn cập nhật count sau mỗi QueryTree thành công. Regression: đóng một cửa sổ giữa stack rồi damage một cửa sổ translucent còn lại; so pixel và số lượt composite.

## 2. HIGH — shape polling có thể tạo busy loop với cửa sổ chưa có picture

**Vị trí:** `src/comp.c:731-758`, scheduler `1292-1310`.

`shape_poll()` bỏ qua `w->pict == None` mà không tăng `shape_checked`. `shape_next_wake()` không bỏ qua cùng điều kiện: sau mốc 100ms nó trả 0 mãi. Vòng lặp select timeout 0 rồi shape_poll lại bỏ qua, gây spin.

Có thể gặp khi resize/remap một cửa sổ đang bị cửa sổ opaque khác che kín: picture bị invalidate nhưng occlusion culling bỏ qua việc realize picture cho cửa sổ phía dưới.

Harness với mapped window, born quá 100ms, picture None:

```
next_wake=0 poll=0 checked_after=0
```

**Sửa:** thống nhất điều kiện eligibility giữa poll và next_wake; không schedule shape recheck cho picture chưa realize, khởi động lại lịch khi realize. Test thêm cửa sổ bị che kín và CPU idle.

## 3. HIGH — XPresent Complete không có nghĩa pixmap đã được phép ghi lại

**Vị trí:** `src/comp.c:287-300`, `1239-1243`, `1490-1508`.

Chỉ subscribe `PresentCompleteNotifyMask`; complete về thì hạ `present_pending` và cho repaint ghi lại chính `back_buf`. Với flip path, pixmap đã hiển thị vẫn có thể đang được scanout. Điều kiện cho phép reuse là IdleNotify/idle fence, không phải chỉ CompleteNotify. Vì chỉ có một back buffer, việc vẽ lại có thể gây tearing hoặc frame dở dang trên driver dùng flip.

**Trạng thái:** xác định từ source và semantics Present; chưa tái hiện trên GPU thật. Copy-mode server có thể che giấu lỗi này.

**Sửa:** dùng pool 2–3 pixmap, subscribe IdleNotify và quản lý busy state theo pixmap/serial; chỉ render vào buffer idle. Nếu không triển khai lifecycle này thì dùng copy presentation hoặc disable flip như biện pháp tạm thời có kiểm chứng.

## 4. MEDIUM — timeout XPresent 200ms không được đưa vào select deadline

**Vị trí:** `src/comp.c:287-300`, `1294-1317`.

Khi pending, scheduler đặt budget=-1 để chờ complete. Deadline 200ms không được đưa vào budget. Nếu complete mất và không có event khác, vòng lặp chỉ thức bởi GC deadline (tới 5 giây), nên fallback không xảy ra đúng 200ms.

**Trạng thái:** source-confirmed; chưa fault-inject mất completion trên Xorg.

**Sửa:** khi pending, thêm deadline `present_sent_ms + PRESENT_TIMEOUT_MS` vào min timeout; xử lý timeout cả khi chưa có dirty/fading. Khi fallback phải tránh ghi vào pixmap còn server sở hữu.

## 5. MEDIUM — shape rỗng hoặc một rectangle lớn bị nhận nhầm là không shaped

**Vị trí:** `src/comp.c:325-350`.

- `n <= 0` bị coi là unshaped, nhưng explicit empty bounding shape phải không vẽ gì.
- Một rectangle có diện tích >= 50% footprint bị coi là unshaped dù không bằng footprint.

Harness trên window 100x100:

```
75-percent rectangle shape detected=0 (expected 1)
empty shape detected=0 (expected shaped empty)
```

Lỗi còn tác động occlusion: một shaped depth-24 window có thể bị coi là opaque phủ kín box, khiến cửa sổ dưới không được vẽ.

**Sửa:** phân biệt query failure với empty shape; xác định shaped bằng Shape extents/đối chiếu hình chữ nhật thực tế, không dùng ngưỡng diện tích. Bảo đảm render loop skip explicit empty shape. Test shape 75%, shape lệch gốc, empty shape.

## 6. MEDIUM — trap bắt được lỗi nhưng caller vẫn cache resource XID không tồn tại

**Vị trí:** `src/comp.c:254-273`, `678-700`; tương tự `win_add():435-437`.

Các hàm tạo X resource của Xlib trả XID được cấp ở client, kể cả khi request sau đó thất bại ở server. `untrap_errors()` chỉ log, không trả trạng thái; caller kiểm tra `!pm`/`!pict` nên không nhận biết failure.

Harness: gọi `ensure_win_pict()` trên window vừa unmap:

```
trapped X error code=8 (BadMatch)
trapped X error code=9 (BadDrawable)
ensure_win_pict on unmapped window=1 cached-pict=0x200012 (expected 0/None)
```

Picture giả được cache và `ensure_win_pict` tiếp theo trả thành công ngay. Race map/unmap hoặc BadAlloc có thể gây mất nội dung, lỗi lặp cho tới lần invalidate tiếp theo.

**Sửa:** trap trả error status, caller chỉ commit resource khi request thành công; xử lý rollback. Đồng thời flush/sync lỗi cũ trước khi mở outer trap để không gán nhầm lỗi request trước cho thao tác đang kiểm tra.

## 7. LOW — so mã BadDamage thiếu extension error base

**Vị trí:** `src/comp.c:1067`.

`BadDamage` trong damagewire.h bằng 0 (offset), không phải wire error code. Cần so `dmg_error + BadDamage`. Hiện race destroy/damage vẫn log production dù code comment nói chỉ debug.

Harness hủy window rồi cleanup Damage in ra:

```
dani-comp: Xerror BadDamage (invalid Damage parameter) req=143/2 ...
```

**Sửa:** cộng đúng error base; giữ các lỗi ngoài allowlist được log.

## Thiếu sót test hiện có

`test/test-comp.sh` chủ yếu kiểm tra process sống, window visible theo server state và screenshot thay đổi; không xác nhận đúng pixel alpha/shape/stack. So MD5 file PNG có thể nhạy với metadata chứ không chỉ pixel. Không có test cache shrink, empty shape, trap failure, occluded-unrealized idle hoặc Present buffer lifecycle.

Lệnh Xvfb dùng `+extension FIXES` cũng sai tên extension (`XFIXES`); server hiện tại vẫn chạy vì extension mặc định được bật.

## Thứ tự đề xuất

1. Sửa count/capacity cache và busy loop shape.
2. Sửa trap status và shape semantics; thêm regression kiểm tra pixel.
3. Sửa Present buffer lifecycle rồi timeout; kiểm tra trên Xorg/GPU thật.
4. Sửa BadDamage error base và tên extension trong test.
