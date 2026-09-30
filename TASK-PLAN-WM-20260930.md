# Plan fix — AUDIT-WM-20260930

Nguồn: `AUDIT-WM-20260930-030823.md` (commit `734fe50`).
Quy tắc chung mỗi task:

1. Viết test hồi quy **trước** (phải FAIL trên code hiện tại), đặt ở `test/`, thêm vào danh sách trong `test/run-all.sh`.
2. Sửa code tối thiểu.
3. `make && make check` → `RESULT: ALL PASS`.
4. Commit riêng mỗi task: `fix(<module>): <mô tả> (audit-0930 #N)`.

Thứ tự: P0 → P1 → P2 → P3. Các task trong cùng phase độc lập nhau.

---

## P0 — ảnh hưởng hằng ngày

### T1 (#1) Focus timestamp cũ → XSetInputFocus bị server bỏ qua

**Nguyên nhân:** `last_evtime` sống mãi; mọi lần focus về sau (pager, manage, unmanage, view) dùng lại timestamp cũ. X server bỏ qua `SetInputFocus` có time < last-focus-change-time.

**Hướng fix (event-scoped timestamp):** timestamp chỉ có hiệu lực trong đúng lượt xử lý event sinh ra nó; ngoài ra dùng `CurrentTime` (giống dwm).

`src/main.c`, ngay sau `XNextEvent(dpy, &ev);`:
```c
XNextEvent(dpy, &ev);
last_evtime = 0; /* timestamp chỉ sống trong 1 lượt dispatch (audit-0930 #1) */
```
Giữ nguyên các chỗ gán `last_evtime = e->time` ở KeyPress/ButtonPress/EnterNotify.

Nhánh `_NET_ACTIVE_WINDOW` (`main.c` ~543): pager gửi timestamp ở `data.l[1]`; dùng nếu khác 0:
```c
if (e->message_type == A_NET_ACTIVE_WINDOW) {
    if (e->data.l[1]) last_evtime = (Time)e->data.l[1];
    if (c) { ...
```
> Lưu ý: nếu sau khi fix vẫn thấy focus bị bỏ qua với một pager cụ thể (timestamp pager cũ hơn), bỏ dòng dùng `data.l[1]` → luôn `CurrentTime`. Ưu tiên "chạy đúng" hơn tuân ICCCM tuyệt đối.

Cập nhật comment ở `state.h:15-19` cho đúng ngữ nghĩa mới.

**Test:** `test/test-focus-time.sh` + `test/focus-time.py` (Phụ lục A1). Kỳ vọng: sau pager activate B → `XGetInputFocus == B`.
**Rủi ro:** `test-takefocus`, `test-focus-steal`, `test-kill-stamp` phụ thuộc timestamp — chạy lại cả bộ. `test-kill-stamp` kiểm tra `WM_DELETE` mang time của KeyPress: vẫn đúng vì `kill_sel` chạy trong lượt KeyPress.

---

### T2 (#2) `focus = click`: click vào cửa sổ tiled không focus

**Hướng fix:** `src/main.c` `xi2_raw_click()`, thay dòng
```c
if (!c->floating && !c->fullscreen) return; /* tiled: no stacking to fix */
```
bằng
```c
if (!c->floating && !c->fullscreen) {
    /* tiled: không cần raise, nhưng click-focus phải chuyển focus (#2) */
    if (FOCUS_MODE != 0 && c != sel) focus_noraise(c);
    return;
}
```
Và ở case `GenericEvent`, trước `xi2_raw_click(re->detail);` gán `last_evtime = re->time;` (kết hợp T1: time sống trong lượt này).

Không thêm passive grab `Button1` không modifier — đã có lý do trong comment (ReplayPointer loop trên Xorg).

**Test:** `test/test-click-focus.sh` (Phụ lục A2). `focus = click`, 2 xterm tiled, click trái → active = master, click phải → active = stack. Thêm trường hợp `focus = hover` để chắc hành vi cũ không đổi.

---

## P1 — sai state

### T3 (#3) `is_sticky` sai do sign-extension

`src/main.c:222`:
```c
if (rf == 32 && n == 1 &&
    ((*(unsigned long *)data) & 0xFFFFFFFFUL) == 0xFFFFFFFFUL) sticky = 1;
```
Grep các chỗ khác đọc CARDINAL rồi so với hằng 32-bit: `grep -n "0xFFFFFFFF" src/*.c` — `ewmh_read_desktop` đọc `long` rồi check `v >= 0`, với -1 vẫn đúng (bỏ qua), không cần sửa.

**Test:** mở rộng `test/test-restart.sh`: park scratch → restart → Super+s → assert `xdotool search --classname scratchpad | wc -l` == 1 (Phụ lục A3).

---

### T4 (#4) Cửa sổ đã withdraw bị map lại sau restart

**Hướng fix (khuyên dùng):** dựa vào `WM_STATE` — daniwm đã set `NormalState` khi manage và `WithdrawnState` khi unmanage (`ewmh_set_wm_state`). Vòng adopt chỉ nhận cửa sổ ẩn có `WM_STATE ∈ {Normal, Iconic}`.

Thêm helper trong `src/main.c` (cạnh `is_sticky`):
```c
/* ICCCM WM_STATE: -1 = không có prop */
static long read_wm_state(Window w) {
    Atom rt; int rf; unsigned long n, extra;
    unsigned char *data = NULL;
    long st = -1;
    if (A_WM_STATE == None) return -1;
    if (XGetWindowProperty(dpy, w, A_WM_STATE, 0, 2, False, A_WM_STATE,
        &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32 && n >= 1) st = *(long *)data;
        XFree(data);
    }
    return st;
}
```
Trong vòng adopt, thay:
```c
if (ewmh_read_desktop(kids[i]) < 0 && !is_sticky(kids[i])) continue;
```
bằng:
```c
long wst = read_wm_state(kids[i]);
if (wst != NormalState && wst != IconicState) continue; /* withdrawn/lạ: bỏ */
if (ewmh_read_desktop(kids[i]) < 0 && !is_sticky(kids[i])) continue;
```
Bổ sung (phòng thủ) trong `unmanage()` `src/client.c`, trước `free(c)`:
```c
trap_errors(dpy);
XDeleteProperty(dpy, w, A_NET_WM_DESKTOP); /* EWMH: WM xoá khi withdraw */
untrap_errors(dpy);
```
(`unmanage` cũng chạy khi DestroyNotify → trap để BadWindow chỉ log.) Cân nhắc gộp chung trap với `ewmh_set_wm_state` ngay trên.

**Test:** `test/test-restart-withdrawn.sh` + `test/withdrawn.py` (Phụ lục A4). Kỳ vọng `map_state` giữ 0 (IsUnmapped). Chạy lại `test-restart`, `test-reload-hidden` để chắc cửa sổ ở ws khác + scratch vẫn adopt đúng.

---

### T5 (#5) `setfullscreen` focus cửa sổ ở workspace khác

`src/ewmh.c` cuối `setfullscreen()`:
```c
arrange();
if (c->ws == curws) focus(c);
```
Không cần làm gì thêm: `arrange()` chỉ xử lý `curws`, khi user `view()` sang ws kia fullscreen sẽ áp dụng.

**Test:** `test/test-fs-offws.sh` + `test/fs-offws.py` (Phụ lục A5). Kỳ vọng `_NET_ACTIVE_WINDOW != w` và stderr không có `BadMatch`.

---

## P2 — robustness / hiệu năng

### T6 (#6) Volume sampling block event loop

**Cách nhanh (đủ dùng):** bọc lệnh bằng `timeout` coreutils trong 4 chuỗi `popen` ở `src/sysmon.c`:
```c
popen("timeout 0.5 wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
```
Giới hạn worst-case ~0.5s/tick (pactl: 1s). Backoff 30s đã có khi fail.

**Cách đúng (khuyên nếu còn giật):** sample bất đồng bộ.
- `sys_vol_update()` → nếu không có job: `pipe()+fork()`, con `dup2` stdout vào pipe, `execvp` backend; lưu `vol_fd`, `vol_pid`.
- `main.c` thêm `vol_fd` vào `select()` giống `rfd`; khi readable → `sys_vol_read()` đọc non-blocking, parse (tách hàm parse ra khỏi `vol_try_*`), đóng fd khi EOF.
- Job quá 2s → `kill(vol_pid, SIGKILL)`, đóng fd, backoff.
- Pactl cần 2 lệnh → dùng `sh -c 'pactl get-sink-volume ...; pactl get-sink-mute ...'` trong 1 job.

**Test:** `test/test-vol-hang.sh`: PATH chứa `wpctl`/`amixer`/`pactl` giả `sleep 30`; sau 3s gửi `super+Return` → xterm phải xuất hiện < 2s.

---

### T7 (#7) Coalesce `arrange()`

1. `src/layout.c`: thêm `static int arrange_dirty; void arrange_later(void) { arrange_dirty = 1; } void arrange_flush(void) { if (arrange_dirty) { arrange_dirty = 0; arrange(); } }`, khai báo ở `layout.h`.
2. Trong `arrange()` đầu hàm: `arrange_dirty = 0;` (gọi trực tiếp thì huỷ pending).
3. `src/main.c`: chỉ ở các nhánh do **client** kích hoạt, đổi `arrange()` → `arrange_later()`: ConfigureRequest (`if (c) arrange();`), MapRequest nhánh known-remap, PropertyNotify. Các đường do user (phím, chuột, view) giữ `arrange()` đồng bộ.
4. Vòng lặp: trước khi vào `while (!XPending(dpy))` select → `arrange_flush();`. Và cuối mỗi lượt dispatch: `if (!XPending(dpy)) arrange_flush();`.
5. `keep_docks_on_top()`: `ewmh_hasstate` cho mọi top-level mỗi lần = N round-trip. Cache: chỉ quét khi có PropertyNotify `_NET_WM_STATE` trên override-redirect window, hoặc giới hạn quét 1 lần / arrange (không gọi trong `focus_ex` nếu vừa arrange).

**Đo:** `test/measure-h2.sh` hoặc script spam 500 ConfigureRequest (python-xlib `w.configure(width=...)` loop) — so thời gian xử lý trước/sau, CPU daniwm.
**Rủi ro cao nhất trong plan** — làm sau cùng trong P2, chạy full `make check` + dùng thật 1 ngày.

---

## P3 — edge / cosmetic

### T8 (#8) Gap không trần + monocle không clamp
- `src/keys.c` `k_gapinc`: `if (gap_outer < 64) gap_outer += 2; if (gap_inner < 64) gap_inner += 1;`
- `src/layout.c` `monocle_mon`: tính `int ww = aw - 2*S(BORDER) - g, wh = ah - 2*S(BORDER) - g; if (ww < 1) ww = 1; if (wh < 1) wh = 1;` rồi dùng `(unsigned)ww/wh`.
- Test: nhấn `super+equal` 200 lần ở monocle → cửa sổ vẫn Viewable, stderr không BadValue.

### T9 (#9) rename kẹt `pending_idx`
- `src/rename.c`: lưu `static pid_t pending_pid; static time_t pending_since;` khi fork.
- Thêm `void rename_tick(void)` gọi từ tick 1s (`main.c` cạnh `tray_poll()`): nếu `pending_idx >= 0 && kill(pending_pid, 0) == -1 && errno == ESRCH` → `rename_poll()` (đọc file nếu có, reset `pending_idx`). Thêm hạn cứng 5 phút.
- Lưu ý `rename_poll()` đang return sớm khi `pending_idx < 0` — thứ tự không đổi.
- Test: `rename_cmd = sh -c 'kill -9 $PPID'` (giết sh cha) → Super+Shift+n lần 2 sau 2s không in "already in progress".

### T10 (#10) Tiled client tự restack
- `src/main.c` nhánh tiled ConfigureRequest: bỏ `XConfigureWindow(... CWSibling|CWStackMode ...)`, chỉ gửi synthetic ConfigureNotify.
- Test: có sẵn `test-nested-float` / `test-click-raise-float` phải vẫn pass; thêm case: tiled client gửi `configure(stack_mode=Above)` → float vẫn ở trên (so thứ tự `XQueryTree`).

### T11 (#11) `_NET_CLOSE_WINDOW` bỏ timestamp
- `src/main.c:565`: `if (c) kill_client_ex(c, (Time)e->data.l[0]);` (`0` = CurrentTime, đúng spec).
- Test: mở rộng `test-kill-stamp.sh`: gửi `_NET_CLOSE_WINDOW` với time 12345 → `WM_DELETE_WINDOW` nhận `data.l[1] == 12345`.

---

## Checklist

- [x] T1 focus timestamp (P0) — `98628d8`
- [x] T2 click-focus tiled (P0) — `c4c6abb`
- [x] T3 is_sticky sign-extension (P1) — `24f7901`
- [x] T4 withdrawn adopt (P1) — `d92f517`
- [x] T5 setfullscreen off-ws (P1) — `f41c589`
- [x] T6 volume non-blocking (P2) — `32ec957` (cách "đúng": job async + timeout 2s)
- [x] T7 arrange coalesce (P2) — `a2caf7d` (2000 ConfigureRequest: 450ms/25 tick → 50ms/3 tick;
      bước 5 cache `keep_docks_on_top` chưa làm — chưa cần)
- [x] T8 gap cap + monocle clamp (P3) — `51e2a49`
- [x] T9 rename stuck (P3) — `6053e7a`
- [x] T10 tiled restack (P3) — `67d4ba7`
- [x] T11 close timestamp (P3) — `f887dbe`
- [x] Cập nhật bảng trạng thái trong `AUDIT-WM-20260930-030823.md` §0/§1

`make check` → `RESULT: ALL PASS` (33 suite). Riêng `test-urgency` flaky sẵn (fail ngẫu nhiên
cả trên commit gốc `734fe50`, đã kiểm chứng), không phải hồi quy.

---

## Phụ lục — script tái hiện (dùng làm khung test)

Tất cả chạy với binary `./daniwm` ở repo root, cần `Xvfb xterm xdotool xprop python3-xlib`. Khi đưa vào `test/`, dùng `find_display.sh` + `HOME` tạm như các suite hiện có, và in `PASS:`/`FAIL:` để `run-all.sh` gom.

### A1 — focus timestamp (#1)
```python
# focus-time.py
import time, subprocess
from Xlib import X, display, protocol
d=display.Display(); r=d.screen().root
A=lambda n:d.intern_atom(n)
def mk(n):
    w=r.create_window(10,10,300,200,0,d.screen().root_depth,X.InputOutput,X.CopyFromParent,background_pixel=0)
    w.set_wm_name(n); w.map(); d.sync(); time.sleep(0.8); return w
a=mk("A"); b=mk("B")
subprocess.run(["xdotool","key","super+j"]); time.sleep(2)   # WM ghi last_evtime
a.set_input_focus(X.RevertToPointerRoot, X.CurrentTime); d.sync(); time.sleep(0.5)
ev=protocol.event.ClientMessage(window=b,client_type=A("_NET_ACTIVE_WINDOW"),data=(32,[2,0,0,0,0]))
r.send_event(ev,event_mask=X.SubstructureRedirectMask|X.SubstructureNotifyMask); d.sync(); time.sleep(0.8)
ok = d.get_input_focus().focus.id == b.id
print("PASS: pager activate moves real focus" if ok else "FAIL: pager activate moves real focus")
```
Chạy: `xdotool mousemove 1270 790` trước khi start WM (tránh hover).

### A2 — click focus tiled (#2)
```sh
printf 'focus = click\n' > $H/.config/daniwm/config
HOME=$H ./daniwm & sleep 1
xterm -title A & sleep 1; xterm -title B & sleep 1.5
act() { xprop -root _NET_ACTIVE_WINDOW | awk '{print $NF}'; }
xdotool mousemove 200 400 click 1; sleep 1; L=$(act)
xdotool mousemove 1000 400 click 1; sleep 1; R=$(act)
[ "$L" != "$R" ] && echo "PASS: click focus tiled" || echo "FAIL: click focus tiled"
```

### A3 — scratchpad sau restart (#3)
```sh
xdotool key super+s; sleep 1.5; xdotool key super+s; sleep 1   # park
xdotool key super+ctrl+r; sleep 2
xdotool key super+s; sleep 1.5
n=$(xdotool search --classname scratchpad | wc -l)
[ "$n" -eq 1 ] && echo "PASS: single scratchpad after restart" || echo "FAIL: $n scratchpads"
```

### A4 — withdrawn không bị map lại (#4)
```python
# withdrawn.py  (script shell chạy song song, gửi super+ctrl+r ở giây ~2.5)
import time
from Xlib import X, display
d=display.Display(); r=d.screen().root
w=r.create_window(10,10,300,200,0,d.screen().root_depth,X.InputOutput,X.CopyFromParent,background_pixel=0)
w.set_wm_name("WDTEST"); w.map(); d.sync(); time.sleep(1)
w.unmap(); d.sync(); time.sleep(5)
st = w.get_attributes().map_state
print("PASS: withdrawn stays unmapped" if st == X.IsUnmapped else "FAIL: withdrawn remapped (state=%d)" % st)
```

### A5 — fullscreen off-workspace (#5)
```python
# fs-offws.py
import time
from Xlib import X, display, protocol
d=display.Display(); r=d.screen().root
A=lambda n:d.intern_atom(n)
def cm(win, typ, data):
    ev=protocol.event.ClientMessage(window=win,client_type=A(typ),data=(32,data))
    r.send_event(ev,event_mask=X.SubstructureRedirectMask|X.SubstructureNotifyMask); d.sync(); time.sleep(1)
w=r.create_window(10,10,300,200,0,d.screen().root_depth,X.InputOutput,X.CopyFromParent,background_pixel=0)
w.set_wm_name("FSTEST"); w.map(); d.sync(); time.sleep(1)
cm(r, "_NET_CURRENT_DESKTOP", [1,0,0,0,0])
cm(w, "_NET_WM_STATE", [1,A("_NET_WM_STATE_FULLSCREEN"),0,1,0])
act=r.get_full_property(A("_NET_ACTIVE_WINDOW"),X.AnyPropertyType).value[0]
print("PASS: off-ws fullscreen keeps focus" if act != w.id else "FAIL: off-ws window became active")
```
Kèm: `grep -q BadMatch wm.err && echo FAIL: BadMatch`.
