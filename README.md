# daniwm — minimal X11 tiling WM

Tiling + monocle, bar with sysmon, 5 workspaces by default (1–10 configurable), gaps, EWMH, multi-monitor (Xinerama), rules, scratchpad, autostart, system tray. Sources in `src/` (12 modules).

## Build

```sh
make            # build ./daniwm (strict warnings, fortified)
make check      # build + run all headless suites (needs Xvfb, xterm, xdotool)
sudo make install   # -> /usr/local/bin + xsessions entry + share/daniwm/*.example
                    #    + user config into ~/.config/daniwm/ (never overwrites)
make install-user   # only copy config + autostart.sh to ~/.config/daniwm/ (no overwrite)
```

`make install` never overwrites `~/.config/daniwm/config` / `autostart.sh` —
existing files are kept. With `DESTDIR` set (packaging) the user-config step is
skipped; examples land in `$(PREFIX)/share/daniwm/config.example` +
`autostart.sh.example` for manual copy.

Requires X11 + Xinerama + Xrandr + Xft headers (`libX11-devel libXinerama-devel libXrandr-devel libXft-devel` on Fedora).
`dani-comp` additionally needs Composite/Damage/Render/Fixes (`libXcomposite-devel libXdamage-devel libXrender-devel libXfixes-devel` — usually preinstalled with libX11-devel).

## Run

```sh
Xephyr :1 & DISPLAY=:1 ./daniwm
# real session: exec ~/daniwm/daniwm  (in ~/.xinitrc)
```

## Keys (Super = Mod4)

| Key | Action |
|---|---|
| Super+j / k | focus next / prev |
| Super+z | zoom: focused window → master (master → swap with 2nd) |
| Super+Tab | toggle previous workspace |
| Super+t / m | tiling / monocle |
| Super+Space | toggle tile/monocle |
| Super+Enter / p | xterm / dmenu_run |
| Super+q | kill window |
| Super+h / l | master size -/+ |
| Super+u / i | nmaster -/+ |
| Super+g, Super+-/= | gaps toggle / shrink / grow |
| Super+b | bar toggle |
| Super+1..9,0 | view workspace (up to `workspaces`) |
| Super+Shift+1..9,0 | move window to workspace (follow) |
| Super+Shift+Space | float toggle |
| Super+f | fullscreen toggle (EWMH) |
| Super+s | scratchpad toggle |
| Super+Shift+r | reload config file |
| Super+Ctrl+r | restart WM in place (clients kept, see below) |
| Super+Left drag | move window — tiled: drop onto another tile swaps them (bspwm-style, target highlighted with a thicker border); drop elsewhere floats, cross-monitor drop re-tiles |
| Super+Right drag | resize window (tiling: horizontal = `mfact`, vertical = `cfact` height weight, stays tiled; floating: resizes window geometry) |
| Super+Ctrl+h/j/k/l | move window — tiled: swap with the neighbour (h/l = stack↔master, j/k = up/down in column); floating: 20px (repeat = smooth) |
| Super+Ctrl+Shift+h/l | resize floating width -/+20px (repeat = smooth) |
| Super+Ctrl+Shift+k/j | resize floating height -/+20px (repeat = smooth) |
| Super+Shift+e | quit |

Bar click on `1..N` switches workspace (active = pill + bright text, occupied = bright + dot, urgent = rose).
Task buttons (one per window on this workspace, Awesome-style): left-click focuses,
middle-click closes. Urgent windows glow in the urgent color.
Scroll over workspaces = prev/next ws (wraps), scroll elsewhere = volume up/down, left-click on the volume module = mute toggle
(middle/right-click anywhere on the bar also mutes)
(backend auto: `amixer` → `wpctl` → `pactl`; laptop `XF86Audio*` keys work out of the box).
Bar text is UTF-8 via Xft with per-glyph fallback (Vietnamese, symbols).
Bar right side: Nerd Font icons + values (`CPU MEM BAT VOL DD/MM HH:MM`, custom `·` separators; falls back to `C/M/B/V` letters without a Nerd Font; low battery/MUTE/CPU≥80%/MEM≥80% in the urgent color; battery/volume hidden if unavailable, volume cached 2s; battery/volume icons change with level/charge/mute; layout shows a tile/monocle icon + window count, no brackets)

## Features

- **EWMH**: `_NET_SUPPORTED/CLIENT_LIST/ACTIVE_WINDOW`, `_NET_WM_PID` (WM PID on the
  supporting window, also advertised in `_NET_SUPPORTED`), fullscreen (`_NET_WM_STATE`), window-type float (dialog/utility/toolbar/splash/menu/dropdown/popup/tooltip/notification/combo/dnd + modal), dock handling (`_NET_WM_WINDOW_TYPE_DOCK`), EWMH struts (`_NET_WM_STRUT` / `_NET_WM_STRUT_PARTIAL` / `_NET_WORKAREA`) for external bars/docks (Polybar, Tint2, Lemonbar, etc.), `_NET_ACTIVE/CLOSE_WINDOW` requests, workspaces (`_NET_NUMBER_OF_DESKTOPS` / `_NET_CURRENT_DESKTOP` / `_NET_WM_DESKTOP` / `_NET_DESKTOP_NAMES`, incl. pager `view` + `move_to` requests).
- **Monitors**: per-monitor tiling via Xinerama (fallback: whole screen); new windows go to the pointer monitor; bar lives on monitor 0; workspaces are global; per-monitor strut margins computed automatically. RandR hotplug: replug/reconfigure outputs re-tiles live, no restart.
- **Rules**: config `rule` lines match class/title substring → float / send to ws (default: scratchpad, Gimp, mpv float).
- **Scratchpad**: `Super+s` toggles `xterm -name scratchpad` (2/3 centered float; first press spawns it). A custom `scratch =` command must produce a window with `scratchpad` in its class/name (e.g. `xterm -name scratchpad`, `alacritty --class scratchpad`); otherwise toggle keeps spawning instead of toggling.
- **Autostart**: runs `~/.config/daniwm/autostart.sh` if executable.
- **Compositor** (`dani-comp`, thay picom nếu thích): bóng đổ 3 lớp, fade-in
  160ms, dim cửa sổ inactive (`inactive_dim`, default 0.92), tôn trọng
  `_NET_WM_WINDOW_OPACITY` (`transset` dùng được), công bố `_NET_WM_CM_Sn`
  (nhường nếu đã có picom). Chạy tay `dani-comp [--no-shadow] [--no-fade]
  [--dim 0.5..1]` hoặc `compositor = 1` trong config để daniwm tự spawn;
  action `compositor` (vd `bind = mod+c:compositor`) toggle nóng.
  Test: `./test/test-comp.sh`. Giới hạn: vẽ lên root (có thể nháy nhẹ khi
  resize liên tục), không xử lý shaped window, không vsync thật.
- **Restart**: `Super+Ctrl+r` (action `restart`) execs a fresh binary over the
  running process — all clients survive, keeping workspace (`_NET_WM_DESKTOP`),
  fullscreen, current desktop, focus, and the parked scratchpad. Floating state
  and per-workspace layout/`mfact`/`nmaster` reset to defaults.
  External watchers can detect a re-exec via the `_DANIWM_HEARTBEAT` root stamp
  (PID is kept across exec, and the X server may recycle window IDs).

## Config
`~/.config/daniwm/config` (`$XDG_CONFIG_HOME/daniwm/config` if set), `key = value`, `#` comment.
Missing file → defaults. Bad line → stderr + ignored, WM keeps running.

Scalars: `mod` (super|alt|ctrl), `border`, `border_focus/border_normal`,
`bar_bg/bar_fg/bar_acc/bar_dim` (hex, `#` optional), `bar_h`, `ws_w`,
`scale` (0.5–3.0, default 1.0 — HiDPI multiplier for WM chrome only:
`bar_h`/`ws_w`/`border`/`gap_outer`/`gap_inner`/float-step/drag-deadzone
plus bar font `size=`; fractional ok, e.g. `scale = 1.5`; reload applies live),
`bar_on/gaps_on` (1/true/yes/on), `compositor` (0 default — tự spawn dani-comp),
`shadow`/`fade` (1 default), `inactive_dim` (0.5–1.0, default 0.92),
`focus` (`hover`| `click`|`both`, default `hover` — hover-to-focus vs click-to-focus), `tray` (system tray on/off, default on),
`gap_outer/gap_inner`, `mfact` (0.1–0.9),
`nmaster`, `workspaces` (1–10, default 5),
`ws_names` (positional `Web Code Chat`, indexed `1:Web 2:Code`, quotes keep
spaces: `"My Web"`; per-ws override `ws_name_N = Foo`, N = 1–10; empty =
number fallback; bar cells auto-grow, reload applies live, published via
`_NET_DESKTOP_NAMES`),
`bar_ws_pad` (0–32, default 12 — text inset each side inside a named ws cell),
`ws_icon_N` (N = 1–10 — Nerd Font glyph shown before the ws name/number on the
bar; hidden if the font lacks it; EWMH names stay plain for pagers),
`font` (fontconfig pattern, e.g. `monospace:size=11`, `JetBrainsMono Nerd Font Mono:size=10`;
default `monospace:size=10`, fallbacks built in: Nerd Fonts for icons, then
`Noto Sans`/`DejaVu Sans`/`Sans`; without a Nerd Font the sysmon falls back to `C/M/B/V` letters),
`bar_ws_style` (`pill` default | `underline` | `block` = old full-height fill),
`bar_gap` (0–8, default 3 — spaces around the separator between right-side modules),
`bar_sep_str` (default `·` — right-module separator, empty = spaces only),
`bar_modules` (default `cpu mem bat vol clock` — order + visibility, delete a name to hide),
`clock_fmt` (strftime, default `%d/%m %H:%M` — e.g. `%H:%M` minimal),
`bar_show_title` / `bar_show_layout` (1/0 — left title, layout icon + count),
`bar_show_tasks` (1/0, default 1 — Awesome-style clickable task buttons;
replaces the lone title when on; overflow collapses into a `+N` chip),
`bar_task_w` (0–512, default 0 = auto equal-share; e.g. `160` = fixed 160px
buttons, left-aligned, clicks past them are no-ops),
`bar_task_pad` (0–32, default 6 — text inset inside each task button),
`bar_task_style` (`follow` default = theo `bar_ws_style` | `pill` | `underline` | `block` | `none` = chỉ màu chữ),
`bar_task_active` (nền task đang focus — pill/block/underline; bỏ trống = theo ws active / pill mờ),
`bar_task_active_text` (chữ task đang focus), `bar_task_text` (alias `bar_task_normal` — chữ task thường; urgent vẫn dùng `bar_urgent`),
`bar_pad_l` (0–32, default 0 — left inset before the workspace block;
workspace clicks are remapped so the padding is a no-op),
`bar_pad_r` (0–32, default 8 — right margin after clock/tray; tray icons align to it),
`ico_cpu/ico_mem/ico_bat/ico_vol/ico_mute/ico_clk` (custom icon glyph, e.g. `ico_cpu = C`
for plain letters, empty = no icon; defaults = Nerd Font icons with automatic ASCII
fallback — `ico_clk` falls back to nothing, preserving the old clock look),
vertical padding: text/icons are always vertically centered, so top/bottom air is just
`bar_h` (taller bar = more air; tray icons stay `bar_h-10`, capped at 22px),
`term/menu/scratch` (commands split with `wordexp`, quotes work),
`rename_cmd` (shell cmd for `ws_rename`, default `rofi -dmenu -i -p rename`;
current name on stdin, stdout = new name; env `$DANIWM_WS_CUR`/`$DANIWM_WS_NUM`
available, e.g. `rename_cmd = rofi -dmenu -p "rename ws$DANIWM_WS_NUM"`).

Bar colors: base `bar_bg/bar_fg/bar_acc/bar_dim` cover everything; optional
per-component overrides fall back to those when unset:
`bar_ws_active` (underline — or box in `block` mode — of current ws), `bar_ws_active_text` (label on it),
`bar_ws_occ` (occupied ws label), `bar_ws_empty`, `bar_urgent` (urgent ws + low battery + MUTE),
`bar_sep` (separators + bottom border), `bar_mode` (window count),
`bar_title` (left window title with `…` fallback when crowded, default dim so status wins), `bar_sys` (sysmon values, default bright; icons use the accent color).

System tray: XEmbed (`_NET_SYSTEM_TRAY_Sn`) on the bar's right edge — `nm-applet --indicator`/
`volumeicon`/`cbatticon` dock automatically; a vertical divider separates the tray
from the clock (same chrome as the workspace separator); clicking the tray area
never leaks into workspace view; an ownerless selection is re-acquired automatically
(e.g. after a standalone tray exits); `tray = 0` disables (reload applies live);
`tray_gap` (0–32, default 6 — khoảng cách giữa các icon tray, reload applies live).
Classic XEmbed only — StatusNotifier/AppIndicator apps need `snixembed` bridge.

```ini
workspaces = 3
rule = Gimp:*:float:*
bind = mod+3:ws3
```

`rule = class:title:float:ws` (`*` = any, float = `float`|`tile`, ws 1-based, ≤ `workspaces`).
`bind = mod+key:action` (modifiers `mod/super/alt/ctrl/shift` + X keysym;
actions: `focus_next/prev`, `zoom`, `ws_toggle`, `kill`, `tile/monocle/toggle`, `spawn_term/menu`,
`mfact_dec/inc` (0.025 steps), `nmaster_dec/inc`, `gap/gap_dec/gap_inc`, `bar`,
`move_left/right/up/down` (tiled: swap position in the layout; floating: move 20px),
`resize_w_dec/inc`, `resize_h_dec/inc`,
`vol_up/vol_down/vol_mute` (`amixer set Master 5%+/5%-/toggle`),
`wsN` (view), `mvN` (move + follow), `float`, `fullscreen`, `scratch`,
`ws_rename` (prompt đổi tên ws đang xem qua `rename_cmd`; Esc giữ tên cũ,
Enter trống xóa về số; WM không freeze lúc gõ),
`reload_config`, `restart`, `compositor` (toggle dani-comp), `quit`).
First `rule`/`bind` line replaces the built-in defaults (default binds are
generated for the configured `workspaces`: `1..9,0`).
Missing `wsN`/`mvN` binds are auto-filled with the same convention
(so raising `workspaces` never leaves a ws unreachable), unless their
combo is already taken by one of your binds.
Reload keeps live per-workspace `mfact`/`nmaster` (config values are startup
defaults); shrinking `workspaces` folds extra workspaces into the last one.

## Verify (headless, works from Wayland)

Needs `xorg-x11-server-Xvfb` (or unpack its rpm userspace-side if no sudo).

```sh
./test/verify.sh     # one suite
./test/run-all.sh    # all 9 suites: verify config kill mouse workspaces strut randr tray restart
```

Dựng Xvfb 1280x800, spawn 3 xterm qua `Super+Return`, assert geometry bằng
`xdotool` (master 14,38,681x744; stack x=707; monocle 14,38,1248x744;
fullscreen 0,0,1280x800), đi qua gaps/ws/move-fullscreen/scratchpad,
chụp từng bước vào `test/shots/`. 18/18 PASS.
