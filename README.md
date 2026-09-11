# daniwm — minimal X11 tiling WM

Tiling + monocle, bar with sysmon, 5 workspaces, gaps, EWMH, multi-monitor (Xinerama), rules, scratchpad, autostart. Single file `daniwm.c`.

## Build

```sh
make            # build ./daniwm (strict warnings, fortified)
make check      # build + run all headless suites (needs Xvfb, xterm, xdotool)
sudo make install   # -> /usr/local/bin + xsessions entry (PREFIX/DESTDIR supported)
```

Requires X11 + Xinerama + Xrandr + Xft headers (`libX11-devel libXinerama-devel libXrandr-devel libXft-devel` on Fedora).

## Run

```sh
Xephyr :1 & DISPLAY=:1 ./daniwm
# real session: exec ~/daniwm/daniwm  (in ~/.xinitrc)
```

## Keys (Super = Mod4)

| Key | Action |
|---|---|
| Super+j / k | focus next / prev |
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
| Super+Left drag | move window (promotes tiled → floating on same monitor, re-tiles on cross-monitor drop) |
| Super+Right drag | resize window (tiling: horizontal = `mfact`, vertical = `cfact` height weight, stays tiled; floating: resizes window geometry) |
| Super+Ctrl+h/j/k/l | move floating window 20px (repeat = smooth; promotes tiled → floating) |
| Super+Ctrl+Shift+h/l | resize floating width -/+20px (repeat = smooth) |
| Super+Ctrl+Shift+k/j | resize floating height -/+20px (repeat = smooth) |
| Super+Shift+e | quit |

Bar click on `1..5` switches workspace. `*` = occupied.
Scroll on the bar = volume up/down, middle/right-click = mute toggle
(backend auto: `amixer` → `wpctl` → `pactl`; laptop `XF86Audio*` keys work out of the box).
Bar text is UTF-8 via Xft with per-glyph fallback (Vietnamese, symbols).
Bar right side: `C cpu%  M mem%  B bat%  V vol  HH:MM` (battery/volume hidden if unavailable; volume via `amixer`, cached 2s).

## Features

- **EWMH**: `_NET_SUPPORTED/CLIENT_LIST/ACTIVE_WINDOW`, fullscreen (`_NET_WM_STATE`), window-type float (dialog/utility/splash), dock handling (`_NET_WM_WINDOW_TYPE_DOCK`), EWMH struts (`_NET_WM_STRUT` / `_NET_WM_STRUT_PARTIAL` / `_NET_WORKAREA`) for external bars/docks (Polybar, Tint2, Lemonbar, etc.), `_NET_ACTIVE/CLOSE_WINDOW` requests, workspaces (`_NET_NUMBER_OF_DESKTOPS` / `_NET_CURRENT_DESKTOP` / `_NET_WM_DESKTOP` / `_NET_DESKTOP_NAMES`, incl. pager `view` + `move_to` requests).
- **Monitors**: per-monitor tiling via Xinerama (fallback: whole screen); new windows go to the pointer monitor; bar lives on monitor 0; workspaces are global; per-monitor strut margins computed automatically. RandR hotplug: replug/reconfigure outputs re-tiles live, no restart.
- **Rules**: config `rule` lines match class/title substring → float / send to ws (default: scratchpad, Gimp, mpv float).
- **Scratchpad**: `Super+s` toggles `xterm -name scratchpad` (2/3 centered float; first press spawns it). A custom `scratch =` command must produce a window with `scratchpad` in its class/name (e.g. `xterm -name scratchpad`, `alacritty --class scratchpad`); otherwise toggle keeps spawning instead of toggling.
- **Autostart**: runs `~/.config/daniwm/autostart.sh` if executable.

## Config
`~/.config/daniwm/config` (`$XDG_CONFIG_HOME/daniwm/config` if set), `key = value`, `#` comment.
Missing file → defaults. Bad line → stderr + ignored, WM keeps running.

Scalars: `mod` (super|alt|ctrl), `border`, `border_focus/border_normal`,
`bar_bg/bar_fg/bar_acc/bar_dim` (hex, `#` optional), `bar_h`, `ws_w`,
`scale` (0.5–3.0, default 1.0 — HiDPI multiplier for WM chrome only:
`bar_h`/`ws_w`/`border`/`gap_outer`/`gap_inner`/float-step/drag-deadzone
plus bar font `size=`; fractional ok, e.g. `scale = 1.5`; reload applies live),
`bar_on/gaps_on` (1/true/yes/on), `gap_outer/gap_inner`, `mfact` (0.1–0.9),
`nmaster`, `workspaces` (1–10, default 5),
`font` (fontconfig pattern, e.g. `monospace:size=11`, `JetBrainsMono Nerd Font Mono:size=10`;
default `monospace:size=10`, fallbacks built in; missing glyphs auto-fall-back via
`Noto Sans`/`DejaVu Sans`/`Sans`),
`term/menu/scratch` (commands split with `wordexp`, quotes work).

Bar colors: base `bar_bg/bar_fg/bar_acc/bar_dim` cover everything; optional
per-component overrides fall back to those when unset:
`bar_ws_active` (box of current ws), `bar_ws_active_text` (label on it),
`bar_ws_occ` (occupied ws label), `bar_ws_empty`, `bar_mode` (`[T] 3n`),
`bar_title` (focused window title), `bar_sys` (cpu/mem/bat/vol/clock).

```ini
workspaces = 3
rule = Gimp:*:float:*
bind = mod+3:ws3
```

`rule = class:title:float:ws` (`*` = any, float = `float`|`tile`, ws 1-based, ≤ `workspaces`).
`bind = mod+key:action` (modifiers `mod/super/alt/ctrl/shift` + X keysym;
actions: `focus_next/prev`, `kill`, `tile/monocle/toggle`, `spawn_term/menu`,
`mfact_dec/inc` (0.025 steps), `nmaster_dec/inc`, `gap/gap_dec/gap_inc`, `bar`,
`move_left/right/up/down` (float 20px), `resize_w_dec/inc`, `resize_h_dec/inc`,
`vol_up/vol_down/vol_mute` (`amixer set Master 5%+/5%-/toggle`),
`wsN` (view), `mvN` (move + follow), `float`, `fullscreen`, `scratch`,
`reload_config`, `quit`).
First `rule`/`bind` line replaces the built-in defaults (default binds are
generated for the configured `workspaces`: `1..9,0`).
Reload keeps live per-workspace `mfact`/`nmaster` (config values are startup
defaults); shrinking `workspaces` folds extra workspaces into the last one.

## Verify (headless, works from Wayland)

Needs `xorg-x11-server-Xvfb` (or unpack its rpm userspace-side if no sudo).

```sh
./test/verify.sh     # one suite
./test/run-all.sh    # all suites: verify config kill mouse workspaces strut randr
```

Dựng Xvfb 1280x800, spawn 3 xterm qua `Super+Return`, assert geometry bằng
`xdotool` (master 14,38,681x744; stack x=707; monocle 14,38,1248x744;
fullscreen 0,0,1280x800), đi qua gaps/ws/move-fullscreen/scratchpad,
chụp từng bước vào `test/shots/`. 18/18 PASS.
