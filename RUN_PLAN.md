# RUN_PLAN — dani-run (launcher thay rofi cho daniwm)

Chốt với user (2026-09-15): **tên `dani-run`**, **gộp repo daniwm**, **fav = grid icon + config thêm app tự do**, **scope = Plan only** (chưa code).

## 0. Hiện trạng

- `config`: `menu = rofi -show drun`, `bind = mod+d:spawn_menu`, `rename_cmd = rofi -dmenu ...`
- Suite hiện tại: `daniwm` + `dani-comp` (1 file `src/comp.c`, target riêng trong `Makefile`).
- Bar đã có pattern reuse được: Nerd Font via Xft (`pick_icon()` + fallback ASCII trong `src/bar.c`), màu Rosé Pine, `wordexp` parse lệnh như `menucmd`, `trim/strip_comment` parse `key = value`.

## 1. Target

```
dani-run              # drun mặc định: list dọc fuzzy
dani-run --fav        # favorite app: grid icon
dani-run --power      # power menu: grid icon (reuse UI fav)
```

Bind dự kiến:
```
menu = dani-run
bind = mod+d:spawn_menu
bind = mod+a:exec dani-run --fav
bind = mod+Shift+e:exec dani-run --power
```

## 2. Config — `run.config` (symlink)

- Repo giữ `~/daniwm/run.config` (tracked git).
- User dùng symlink: `~/.config/daniwm/run.config -> /home/dani/daniwm/run.config` (y hệt `config` hiện tại).
- Code chỉ đọc `$XDG_CONFIG_HOME/daniwm/run.config` → fallback `~/.config/daniwm/run.config`, symlink tự resolve.
- `make install-user`: đã tồn tại (file hay symlink) thì `keep existing`, không overwrite.
- Style `key = value`, `#` comment, reuse `trim/strip_comment`. `app`/`power` lặp nhiều dòng như `bind`/`rule` (dòng đầu tiên xóa defaults).

```ini
font = SpaceMono Nerd Font:size=11
cols = 5
lines = 2

# app = icon ; tên ; lệnh (chỉ split 2 dấu ; đầu, lệnh giữ nguyên spaces)
app = ;Firefox;firefox
app = ;Terminal;alacritty
app = 󰨇;Vendors;zdev wcvdev/wc-vendors
app = 󰈙;Files;thunar

power = 󰌾;Lock;loginctl lock-session
power = 󰍃;Logout;pkill daniwm
power = 󰜉;Reboot;systemctl reboot
power = 󰐥;Off;systemctl poweroff
```

- `icon` = 1 glyph Nerd Font, để trống → lấy chữ cái đầu của tên. Reuse `pick_icon()`: thiếu font → fallback ASCII, không tofu.
- `cmd` chạy qua `wordexp` như `menucmd` (vd `zdev wcvdev/wc-vendors` chạy nguyên).

## 3. Kiến trúc

- 1 file `src/run.c` (~800 dòng, Xlib + Xft, không thêm dep), `Makefile` thêm target `dani-run`, `make install` + `install-examples` copy `run.config.example`.
- Data source theo mode:
  - `drun`: quét `/usr/share/applications` + `~/.local/share/applications` (`Name`, `Exec` bỏ `%f/%u`, `Terminal=true` bọc `$TERM -e`, bỏ `NoDisplay/Hidden`).
  - `fav`: parse `app =` từ config.
  - `power`: parse `power =` từ config.
- Fuzzy drun: subsequence case-insensitive + điểm (đầu từ > giữa từ, liền nhau > rời), sort `score + history` (`~/.cache/dani-run/history` đếm frequency).
- UI:
  - drun: cửa sổ center, input trên + list dọc 8 dòng dưới.
  - fav/power: input filter trên + grid `cols x lines`, mỗi ô icon to + label nhỏ, selected = pill iris như `bar_ws_style = pill`.
  - Phím: `↑↓←→/hjkl` di chuyển, `Enter` fork+exec, `Esc` thoát. Fav cũng filter được bằng input.
- Không đổi behavior daniwm/bar/EWMH.

## 4. Milestones

1. `drun` chạy được + fuzzy cơ bản + fork/exec.
2. `--fav` + `--power` (đổi data source, reuse 100% UI grid).
3. History + sort thông minh + theme theo daniwm (font/màu reuse `bar.c`).
4. `run.config.example` + `make install-user` giữ symlink + docs README.

## 5. Câu hỏi còn mở

- `cols` default 5 hay 4 (ô to hơn)?
- Fav có cần ảnh `.png` hay chỉ Nerd glyph là đủ?
