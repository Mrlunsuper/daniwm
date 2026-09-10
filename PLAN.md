# daniwm roadmap

## 1. EWMH compat (làm trước — ít lỗi app/panel nhất)
- Root: `_NET_SUPPORTED`, `_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`.
- Client: tôn trọng `_NET_WM_WINDOW_TYPE` (dialog/dock → float),
  `_NET_WM_STATE_FULLSCREEN` (fill màn hình, bỏ gap/border).
- `ClientMessage`: `_NET_ACTIVE_WINDOW` (focus), `_NET_CLOSE_WINDOW` (kill),
  `_NET_WM_STATE` add/remove/toggle fullscreen.
- `focus()` set `_NET_ACTIVE_WINDOW`; manage/unmanage refresh `_NET_CLIENT_LIST`.

## 2. Multi-monitor (nặng nhất)
- Detect qua Xinerama, fallback 1 màn = whole screen.
- `Client.mon`; manage gán theo vị trí pointer, fallback mon 0.
- `arrange()` chạy tile/monocle riêng từng monitor; monocle: mỗi monitor
  hiện client focus-gần-nhất của nó (`mon_sel[]`).
- Bar + workspaces giữ global (mọi màn cùng ws) cho đơn giản; bar nằm mon 0.

## 3. Rules + scratchpad + autostart (sướng hằng ngày)
- `rules[]` tĩnh: match `class`/`title` substring → float / gửi ws.
- Scratchpad: `Super+s` toggle terminal `xterm -name scratchpad`
  (map giữa màn hình, float; ẩn = unmap).
- Autostart: chạy `~/.config/daniwm/autostart.sh` nếu executable, không block.
