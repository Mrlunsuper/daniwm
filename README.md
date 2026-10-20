# daniwm — minimal X11 tiling WM

Tiling + monocle, bar with sysmon, 5 workspaces by default (1–10 configurable), gaps, EWMH, multi-monitor (Xinerama), rules, scratchpad, autostart, system tray. Sources in `src/` (12 modules).

## Build

```sh
make            # build ./daniwm (strict warnings, fortified)
make check      # build + run all headless suites (needs Xvfb, xterm, xdotool)
sudo make install   # -> /usr/local/bin + xsessions entry + share/daniwm/*.example
                    #    + user config into ~/.config/daniwm/ (never overwrites)
make install-user   # only copy config + autostart.sh + run.config to ~/.config/daniwm/ (no overwrite)
```

`make install` never overwrites `~/.config/daniwm/config` / `autostart.sh` / `run.config` —
existing files are kept. With `DESTDIR` set (packaging) the user-config step is
skipped; examples land in `$(PREFIX)/share/daniwm/config.example` +
`run.config.example` + `autostart.sh.example` for manual copy.

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
| Super+Enter / p | terminal / dani-run (drun) |
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
| Super+Shift+p | power menu (dani-run --power) |
| Super+Shift+e | quit |

Bar click on `1..N` switches workspace (active = pill + bright text, occupied = bright + dot, urgent = rose).
Task buttons (one per window on this workspace, Awesome-style): left-click focuses,
middle-click closes. Urgent windows glow in the urgent color.
Scroll over workspaces = prev/next ws (wraps), scroll elsewhere = volume up/down, left-click on the volume module = mute toggle
(middle/right-click anywhere on the bar also mutes)

Pointer focus & stacking: hover (sloppy) focuses — moving the pointer over a window
activates it without restacking (a small floating window nested inside a big one stays
reachable). Plain clicks never get stolen from apps: the WM observes them via XI2 raw
events, so a click on a floating window brings it to the front (click-to-raise) while
the app still receives every button event (press and release) untouched. `Super+click`
still does move/resize drags, and `Super+j/k` cycle focus with raise.
(backend auto: `amixer` → `wpctl` → `pactl`; laptop `XF86Audio*` keys work out of the box).
Bar text is UTF-8 via Xft with per-glyph fallback (Vietnamese, symbols).
Bar right side: Nerd Font icons + values (`CPU MEM BAT VOL DD/MM HH:MM`, custom `·` separators; falls back to `C/M/B/V` letters without a Nerd Font; low battery/MUTE/CPU≥80%/MEM≥80% in the urgent color; battery/volume hidden if unavailable, volume cached 2s; battery/volume icons change with level/charge/mute; layout shows a tile/monocle icon + window count, no brackets)

## Features

- **EWMH**: `_NET_SUPPORTED/CLIENT_LIST/ACTIVE_WINDOW`, `_NET_WM_PID` (WM PID on the
  supporting window, also advertised in `_NET_SUPPORTED`), fullscreen (`_NET_WM_STATE`), window-type float (dialog/utility/toolbar/splash/menu/dropdown/popup/tooltip/notification/combo/dnd + modal), dock handling (`_NET_WM_WINDOW_TYPE_DOCK`), EWMH struts (`_NET_WM_STRUT` / `_NET_WM_STRUT_PARTIAL` / `_NET_WORKAREA`) for external bars/docks (Polybar, Tint2, Lemonbar, etc.), `_NET_ACTIVE/CLOSE_WINDOW` requests, workspaces (`_NET_NUMBER_OF_DESKTOPS` / `_NET_CURRENT_DESKTOP` / `_NET_WM_DESKTOP` / `_NET_DESKTOP_NAMES`, incl. pager `view` + `move_to` requests). EWMH client messages are trust-based per spec: any client may request close/move/focus for any window (pagers and `wmctrl` rely on this), so daniwm honors them without sender checks.
- **Monitors**: per-monitor tiling via Xinerama (fallback: whole screen); new windows go to the pointer monitor; bar lives on monitor 0; workspaces are global; per-monitor strut margins computed automatically. RandR hotplug: replug/reconfigure outputs re-tiles live, no restart.
- **Rules**: config `rule` lines match class/title substring → float / send to ws (default: scratchpad, Gimp, mpv float).
- **Scratchpad**: `Super+s` toggles `xterm -name scratchpad` (2/3 centered float; first press spawns it). A custom `scratch =` command must produce a window with `scratchpad` in its class/name (e.g. `xterm -name scratchpad`, `alacritty --class scratchpad`); otherwise toggle keeps spawning instead of toggling.
- **Autostart**: runs `~/.config/daniwm/autostart.sh` if executable.
- **Compositor** (`dani-comp`, a picom alternative if you like): soft 7-layer drop shadow (picom-style), 160ms fade-in,
  inactive window dim (`inactive_dim`, default 0.92), honors
  `_NET_WM_WINDOW_OPACITY` (works with `transset`), advertises `_NET_WM_CM_Sn`
  (yields if picom is already running). Run `dani-comp [--no-shadow] [--no-fade]
  [--dim 0.5..1]` manually or set `compositor = 1` in config so daniwm spawns it;
  the `compositor` action (e.g. `bind = mod+c:compositor`) toggles it live.
  Test: `./test/test-comp.sh`. Limits: double-buffered (no flicker), bg snapshot
  from `_XROOTPMAP_ID` (a solid-color root falls back to 1 clear on bg change
  only), shaped windows clipped on the back buffer, no real vsync.
- **Launcher** (`dani-run`, a rofi replacement): `dani-run` = vertical fuzzy drun list
  (scans `/usr/share/applications` + `~/.local/share/applications`, wraps `Terminal=true`
  in `${TERMINAL:-alacritty} -e`, fuzzy sort + history at `~/.cache/dani-run/history`, and
  shows each app's real icon from the icon theme — PNG via cairo when available, Nerd
  glyph/first-letter fallback otherwise; `drun_icons = 0` turns the icons off);
  `dani-run --fav` = favorite-app icon grid (filterable); `dani-run --power` =
  power-menu grid (reuses the fav UI); `dani-run --calc` = calculator
  (`+ - * / % ^`, parens, `sqrt/sin/cos/tan/exp/abs/ln/log`, `pi`/`e`;
  `Enter` copies the result to clipboard + prints to stdout). Config at `~/.config/daniwm/run.config`
  (symlinked to the repo, `app = icon;name;cmd`, `power = ...`, empty icon → first
  letter; `cols`/`lines`/`font`; the first `app`/`power` line clears defaults). Keys:
  `↑↓←→`/`Ctrl+hjkl` to move, `Enter` to run
  (unmatched drun input runs as a command), `Esc` to quit, mouse scroll = prev/next.
  Note this repo's own config uses a full `bind =` list (e.g. `mod+d` = menu,
  `mod+Shift+q` = kill) + `bind+` to add (`mod+a` = fav,
  `mod+Shift+p` = power) without losing existing keys.
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
`bar_ws_task_gap` (0–64, default 12 — dead px between the ws/layout block and
the tasklist, with a thin divider drawn in the middle when tasks show;
clicks there are no-ops, so a wider value stops ws/task mis-clicks),
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

`rule = class:title:float:ws[:layout]` (`*` = any, float = `float`|`tile`, ws 1-based, ≤ `workspaces`; optional layout = `tile`|`monocle`).
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

## AI chat (`dani-ai`, BYOK, Python stdlib)

```sh
dani-ai "cau hoi"        # hoi nhanh (stream mac dinh)
dani-ai chat             # REPL: /reset /system /model /base /stream /color /quit
dani-ai chat --continue [ten]  # mo lai chat cu (tu dong luu last.json)
dani-ai models           # list model tu API (/model de chon so trong chat)
dani-ai history          # list chat da luu (/save [ten] trong chat)
```

Key: `--api-key` > `DANI_AI_API_KEY` > `OPENAI_API_KEY` > `DEEPSEEK_API_KEY` >
`~/.config/daniwm/ai.key` (3 dong: key, base, model). System: `--system` >
`--system-file` > `~/.config/daniwm/ai.system`. Markdown render mau khi chay
trong terminal, giu text tho khi pipe (`--no-color` de tat). Phim tat:
`Super+Shift+a`. Test: `./test/test-ai.sh` (bao gom regression bao mat/loi,
khong can API key that).

### Paste anh / text trong chat

Nhan **Ctrl+V** de doc clipboard truc tiep (khong can sua terminal):
- Anh PNG/JPEG/WebP/GIF: hien `[Ảnh 1]`, co the paste nhieu anh, go cau hoi roi Enter de gui.
- Text: chen vao vi tri con tro. Xoa marker anh truoc Enter de bo dinh kem.
- Can GNU readline va `xclip` (X11) hoac `wl-paste` (Wayland); moi anh toi da 10 MiB.
- Model/API phai ho tro vision va OpenAI-compatible `image_url`.
- Anh duoc gui toi provider khi Enter va luu trong session (base64, file quyen 0600).
  Ctrl+Shift+V van la paste text cua terminal, khong phai phim paste anh.
  GNU readline bat bracketed paste: text nhieu dong (ke ca newline cuoi)
  duoc chen de xem/sua, khong tu gui; nhan Enter sau khi paste de gui.
  Can terminal ho tro bracketed paste; terminal khong ho tro van co the
  coi newline trong clipboard la Enter.

### Tool use (bat mac dinh)

Tool builtin mac dinh: `list_files`, `read_file`, `search_text`.
**`run_command` tat mac dinh**; chi bat bang `--allow-shell`.
Model tu quyet dinh khi nao goi tool; provider/model can ho tro OpenAI function calling.

**Canh bao:** sau khi bat `--allow-shell`, `run_command` chay `/bin/sh -c`
voi quyen user hien tai, khong hoi xac nhan tung lenh. Co the ghi/xoa file, truy cap mang va doc duong dan ngoai root.
`--tool-root` chi la working directory cua shell, KHONG phai sandbox.
Chi su dung voi model/provider va noi dung ban tin tuong. Chan `run_command`
neu muon chi doc file. Blocklist theo ten tool khong phai bo loc lenh shell.

```sh
dani-ai chat --tool-root ~/project
dani-ai chat --allow-shell         # opt-in shell KHONG sandbox
dani-ai "Doc README.md va tom tat"
dani-ai chat --block-tool read_file --block-tool search_text
dani-ai chat --tool-blocklist ./blocked-tools.txt
dani-ai chat --no-tools            # provider khong ho tro / tat toan bo
dani-ai --show-config              # xem tools dang bat va root
```

Blocklist mac dinh: `~/.config/daniwm/ai.tools.blocklist`
(ton trong `XDG_CONFIG_HOME`). Moi dong mot ten/glob; dong trong va `#` bo qua:

```text
# Tat shell truoc khi gioi han truy cap file
run_command
read_file
search_*
# Dung * de chan tat ca
```

`--block-tool` co the lap lai, cong don voi file blocklist. Tool bi chan khong
xuat hien trong request; neu model van goi thi executor tu choi.
Blocklist duoc doc khi khoi dong, can mo lai CLI sau khi thay doi.

`run_command` co timeout 30 giay, gioi han 64 KiB output stdout+stderr;
tra exit code va ly do dung. CLI don process group khi ket thuc/Ctrl-C.
API key va bien moi truong co ten secret/token/password khong duoc truyen
cho shell, nhung shell van co the doc file user: day khong phai cach ly bao mat.
Vi du: `dani-ai --allow-shell "Chay uname -a va df -h, tom tat ket qua"`.
`--yes-tools` khong bat shell; blocklist van uu tien hon `--allow-shell`.

Root mac dinh la cwd. Cac tool file (khong bao gom shell) khong cho duong dan tuyet doi, `..`, symlink,
file an, mot so ten secret/key pho bien; khong doc hardlink hay device/FIFO.
Doc toi da 64 KiB/file, tra toi da 16.000 ky tu/tool file; toi da 32 vong tool.
Custom tool giu nguyen output trong gioi han `output_limit` cua manifest.
Response co `finish_reason: length` hoac `content_filter` duoc bao loi,
khong luu cau tra loi do dang vao history (stream co the da hien text mot phan).
`search_text` tim chuoi literal trong mot file, khong de quy.
Ket qua file duoc gui den API provider: chi dung root chua du lieu ban muon
chia se. Bo loc ten secret khong the nhan dien moi du lieu nhay cam.
Neu can cam doc noi dung, chan `run_command`, `read_file` va `search_text`.

### Gold / XAUUSD proxy (khong can key)

Tool `ai.tools/xauusd_market.json` + `ai.tools/xauusd_market.py` lay quote va
OHLCV tu Yahoo Finance **GC=F (COMEX gold futures)**. Day **KHONG phai
XAUUSD spot**; gia co the tre, chenh basis va bi anh huong boi futures roll.
Khong dat lenh, khong co funding/OI. Ket qua ghi timestamp, quote age va
canh bao; khong dung lam execution feed. Yahoo co the rate-limit/block.

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/daniwm/ai.tools"
cp ai.tools/xauusd_market.* "${XDG_CONFIG_HOME:-$HOME/.config}/daniwm/ai.tools/"
dani-ai "Dung xauusd_market lay 100 nen 1h, nhan xet vang va ghi ro day la futures proxy"
```

Intervals: `1m`, `5m`, `15m`, `30m`, `1h`, `1d`, `1wk`, `1mo`;
`limit` 1..200 (mac dinh 60). Nen cuoi co the chua dong; volume la futures.

### Custom tools (tu dinh nghia)

Ngoai 4 tool builtin, dani-ai nap them manifest JSON trong
`~/.config/daniwm/ai.tools/` (`$XDG_CONFIG_HOME` duoc ton trong).
Moi tool = 1 file `<ten>.json` + 1 script/lenh bat ky (sh, python, binary...).
Khong can sua dani-ai khi them tool moi.

```sh
dani-ai tools                     # liet ke tool builtin + custom
dani-ai --tools-dir ./ai.tools tools
dani-ai --no-custom-tools chat    # chi dung builtin
dani-ai --yes-tools chat          # tu dong dong y tool co confirm
```

Manifest toi thieu:

```json
{
  "name": "btc_market",
  "description": "Read public Binance USD-M futures data (ticker, klines, funding, OI, long/short). Read-only.",
  "command": ["python3", "{manifest_dir}/btc_market.py"],
  "timeout": 30,
  "output_limit": 131072,
  "confirm": false,
  "parameters": {
    "type": "object",
    "properties": {"symbol": {"type": "string"}},
    "required": [],
    "additionalProperties": false
  }
}
```

Hop dong thuc thi:

- Model goi tool voi JSON arguments; dani-ai **validate theo `parameters`**
  (JSON Schema con: `type`, `enum`, `properties`, `required`,
  `additionalProperties`, `items`, `minItems`/`maxItems`, `minLength`/`maxLength`,
  `minimum`/`maximum`). Sai -> tra loi `error` cho model, khong chay lenh.
- Arguments duoc gui qua **stdin** (JSON, khong qua shell, khong interpolate).
  Tool in JSON (hoac text) ra stdout; stderr duoc giu rieng (toi da 4 KiB).
  Exit code != 0 -> `error` + `stderr`; stdout rong -> `error`.
- `command` la argv list, chay truc tiep (khong qua `/bin/sh`) voi cwd =
  tool root. `{manifest_dir}` duoc thay bang thu muc chua manifest.
- Bien moi truong co ten `*API_KEY*`, `*TOKEN*`, `*SECRET*`, `*PASSWORD*`,
  `*CREDENTIAL*` **khong duoc truyen** cho tool (giong `run_command`).
  Them `DANI_TOOL_NAME`, `DANI_TOOL_ROOT`.
- `timeout` (mac dinh 30s, toi da 120s), `output_limit` (256..262144 bytes,
  mac dinh 64 KiB). Vuot -> kill process group, tra `error`.
  JSON hop le duoc giu nguyen cau truc; khong cat ngam o 16.000 ky tu.
- `confirm: true` -> hoi `y/N` tren TTY truoc khi chay (cho tool co side
  effect: ghi file, dat lenh, goi API thay doi du lieu). Khong co TTY hoac
  `--yes-tools` thi quyet dinh tuong ung (tu choi / tu dong dong y).
- Ten tool phai khop `^[a-z][a-z0-9_]{1,40}$`, khong duoc trung builtin;
  toi da 32 custom tool. Manifest loi bi bo qua va bao trong
  `dani-ai --show-config` (`tool_manifest_error = ...`).
- `--block-tool` / blocklist ap dung cho ca custom tool (glob theo ten).

Tool BTC tra `long_pct`/`short_pct` theo phan tram (0..100).
Long/short period khac kline interval: `1m` -> `5m`, `8h` -> `6h`,
`1w` -> `1d`; period thuc te nam trong `long_short.period`.

Tool mau: `ai.tools/btc_market.json` + `ai.tools/btc_market.py` lay du lieu
cong khai Binance USD-M futures (ticker 24h, klines OHLCV + thong ke cua so,
funding rate, open interest, long/short ratio) — chi doc, khong can API key,
khong dat lenh. Copy vao config de dung:

```sh
mkdir -p ~/.config/daniwm/ai.tools
cp ~/daniwm/ai.tools/* ~/.config/daniwm/ai.tools/
dani-ai "Lay du lieu BTC 4h va nhan xet xu huong"   # model tu goi btc_market
```

### Do tin cay va bao mat

- `--continue last "cau hoi tiep"` gui ca context da luu trong single-shot.
  Single-shot khong tu ghi de session; REPL tu luu khi thoat.
- Terminal hien spinner `Đang chờ phản hồi…` cho den khi co noi dung tra loi
  (ca stream va non-stream); tu don dep khi loi/Ctrl-C. Pipe khong co spinner.
- Loi API/timeout hoac Ctrl-C khi dang request khong dong REPL. Luot loi bi
  loai khoi context; co the nhap lai. Cac luot thanh cong van duoc autosave.
- Stream phai ket thuc bang `[DONE]`. JSON loi, error event hoac stream bi cat
  duoc bao loi; phan text da hien thi khong duoc luu nhu cau tra loi hoan chinh.
  Token duoc hien thi ngay, khong doi newline; mau inline duoc render theo chunk.
- History va readline history duoc ghi atomic: thu muc `0700`, file `0600`.
  Day van la plaintext, khong phai ma hoa. File session cu chi duoc thay doi
  quyen khi ghi lai.
- Redirect khac origin (scheme/host/port), ke ca HTTPS sang HTTP, bi chan de
  tranh gui API key sang endpoint khac. HTTP van duoc ho tro cho local server;
  hay dung HTTPS cho endpoint remote.
- Control sequence tu API bi loc khi hien thi TTY; pipe giu raw text.
  `--no-color`/`NO_COLOR` tat mau do ung dung them, khong tat bo loc TTY.

## Verify (headless, works from Wayland)

Needs `xorg-x11-server-Xvfb` (or unpack its rpm userspace-side if no sudo).

```sh
./test/verify.sh     # one suite
./test/run-all.sh    # all suites: verify config kill mouse workspaces strut randr tray
#                    # restart tasklist comp nested-float click-delivery click-raise-float
#                    # focus-steal ghost-unmap wm-state focus-model noinput takefocus
#                    # remap-steal xerr direct-state urgency ws-storm kill-stamp
#                    # clamp-float reload-hidden supported
```

Dựng Xvfb 1280x800, spawn 3 xterm qua `Super+Return`, assert geometry bằng
`xdotool` (master 14,38,681x744; stack x=707; monocle 14,38,1248x744;
fullscreen 0,0,1280x800), đi qua gaps/ws/move-fullscreen/scratchpad,
chụp từng bước vào `test/shots/`. 18/18 PASS.
