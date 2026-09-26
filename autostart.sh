#!/usr/bin/env bash
# Load X resources TRƯỚC khi spawn app (như bspwmrc) — nếu thiếu dòng này
# Xft.dpi=144 trong ~/.Xresources không bao giờ apply -> Thunar/Telegram bé tí.
xrdb -merge "$HOME/.Xresources" 2>/dev/null

# HiDPI 4K 27" ~1.5x (khớp Xft.dpi=144). Đừng để GDK_DPI_SCALE=1.
export GDK_SCALE=1
export GDK_DPI_SCALE=1.5
export QT_AUTO_SCREEN_SCALE_FACTOR=1
export QT_SCALE_FACTOR=1.5
export QT_FONT_DPI=144

run_once() {
  local process_name="$1"
  shift

  pgrep -u "$USER" -x "$process_name" >/dev/null 2>&1 || "$@" &
}

# Tất cả bên dưới chạy detached: daniwm đã sẵn sàng sau ~17ms, script này
# không được giữ chân tiến trình cha. xrdb ở trên vẫn chạy trước mọi app.
{
run_once polkit-mate-authentication-agent-1 /usr/libexec/polkit-mate-authentication-agent-1
run_once fcitx5 fcitx5
run_once nm-applet nm-applet
run_once xfce4-power-manager xfce4-power-manager
run_once blueman-applet blueman-applet
run_once clipit clipit
#run_once eww "$HOME/.local/bin/eww" daemon

# dani-comp (compositor của nhà trồng) — chỉ start 1 instance duy nhất.
# Chỉnh shadow/fade/dim ở đây: daniwm không quản lý compositor nữa.
#run_once dani-comp "$HOME/daniwm/dani-comp" --shadow --no-fade --dim 0
run_once fastcompmgr -o 0.4 -r 12 -c -C
run_once dunst dunst

# Wallpaper lên sớm nhất có thể (feh decode ảnh 3K ~150ms, không chặn ai)
"$HOME/daniwm/set-wallpaper.sh" >/dev/null 2>&1 &

xset r rate 300 30 >/dev/null 2>&1
xsetroot -cursor_name left_ptr >/dev/null 2>&1

# ── NumLock ON (numpad hoạt động như số, không phải navigation) ──
if command -v setxkbmap >/dev/null 2>&1; then
  setxkbmap -option numlock:on >/dev/null 2>&1
fi

# Chậm mà không quan trọng (xset q + fork xinput mỗi device):
# cho vào background, desktop hiện đủ trước rồi mới tune.
{
  # Bật trạng thái NumLock thật sự (setxkbmap chỉ set option, không bật state)
  if command -v xdotool >/dev/null 2>&1; then
    state="$(xset q 2>/dev/null | awk '/Num Lock:/{print $8}')"
    [ "$state" = "off" ] && xdotool key Num_Lock
  fi

  if command -v xinput >/dev/null 2>&1; then
    xinput list --id-only 2>/dev/null | while read -r id; do
      xinput list-props "$id" 2>/dev/null | grep -q "libinput Accel Speed" || continue
      xinput set-prop "$id" "libinput Accel Speed" 0.0 >/dev/null 2>&1
    done
  fi
} &
} &
