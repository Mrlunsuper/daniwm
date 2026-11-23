# daniwm — minimal X11 tiling WM

Tiling + monocle, bar with sysmon, 5 workspaces by default (1–10 configurable), gaps, EWMH, multi-monitor (Xinerama), rules, scratchpad, autostart, system tray. Sources in `src/` (12 modules).

## Three-finger swipe: workspace switching and overview (opt-in)

Enable `swipe_enabled = 1` to use three-finger gestures (default `0` below).
Left goes to the next workspace, right to the previous; up shows the overview
and down hides it without committing a pending selection. While the overview
is open, left/right change only its pending selection; Enter/click commits and
Esc cancels. Repeated up/down are idempotent. Super+O also toggles the overview.
Actions occur only once on a completed, uncancelled, exactly-three-finger gesture.
Short, ambiguous and wrong-finger gestures do nothing. The dominant axis must
meet the threshold and total perpendicular travel must be at most half its net
distance. Meaningful dominant-axis reversal rejects the stroke; minor-axis
reversal alone does not (for either horizontal or vertical gestures).
Disabled by default:

```ini
swipe_enabled = 0
swipe_threshold = 80
swipe_wrap = 0
swipe_reverse = 0
swipe_debug = 0
```

### Opt-in diagnostics (not a runtime fix)

Set `swipe_debug = 1` manually in your config only when investigating missing
swipes; default `0` is silent. Stderr normally goes to `~/.xsession-errors`
(depending on how the X session is launched). Logs contain XI support/selection
status, one Begin and one final decision/reset per gesture, device/source IDs,
finger count, aggregate deltas, threshold, rejection reason and workspace target
(including clamping). Repeated overlapping Begins and Updates are not logged.
No application contents or key events are recorded. Reload, drag and
hierarchy/device changes have distinct reset reasons. A successful selection
without Begin logs can help distinguish lack of delivery from classifier rejection;
it does not by itself establish the runtime cause. XI swipe ownership is exclusive,
so do not run a second swipe observer alongside the WM.

Deployment is **manual**, not performed by the test suite: build with `make daniwm`,
then copy the binary to a temporary file in the same directory as the installed
binary and rename it over the installed binary (for example, if your actual
installed path is `~/.local/bin/daniwm`:
`cp ./daniwm ~/.local/bin/daniwm.new && mv ~/.local/bin/daniwm.new ~/.local/bin/daniwm`).
Use your actual installed path, then press **Super+Ctrl+R** to restart. Enable or
disable the debug scalar yourself and reload/restart as appropriate. These logs
are diagnostic instrumentation, **not a claim that the missing horizontal swipe
bug is fixed**; no live reproduction is required by the offline checks.

`swipe_threshold` is a finite number in 1..10000 (default 80), measured in
accumulated XI gesture delta units, not guaranteed physical pixels. Invalid
values are rejected. `swipe_wrap` permits crossing the first/last workspace;
`swipe_reverse` swaps left/right only; up/down remain show/hide. Reload cancels any in-flight gesture before
loading settings and reconfigures delivery after workspace arrays are rebuilt.

XI2.4 headers/server support and actual touchpad gesture delivery are required;
compilation and offline tests do not establish physical delivery. Root observation
may not deliver over application windows; applications or competing grabs may
consume gestures. The Phase 1 adapter only selects events: it never grabs,
freezes, or replays input. Run `make check-swipe` for its offline tests.
Passive grabs in the separate diagnostic may intercept non-three-
finger swipes too: the XI API has no finger-count grab filter, and rejecting them
in the classifier cannot restore input to applications. Keep this opt-in off
unless delivery and application-input behavior have been verified on an approved
setup. No live-display or hardware verification is claimed here.

### Phase 0 diagnostic reference (preserved)

The following describes the original standalone Phase 0 probe, not the current
Phase 1 WM feature; its evidence requirements and limitations still apply.

**Ở Phase 0 chưa triển khai chuyển workspace hay overview.** Công cụ bên dưới chỉ thu thập
bằng chứng XI2.4; không được nối vào WM/compositor, startup hoặc installation.
Không đổi focus/workspace, map cửa sổ ứng dụng hay replay core button.

Build và kiểm tra offline (không mở display, không cần quyền truy cập touchpad):

```sh
make test/xi2-gesture-probe
make check-xi2-probe
./test/xi2-gesture-probe --build-info
./test/xi2-gesture-probe --self-test
```

Probe bắt buộc `--display NAME`, không tự dùng `$DISPLAY`. Exit code: 0 thành
công, 1 lỗi kết nối/protocol, 2 sai tham số, 3 không hỗ trợ. Header cũ build nhánh
unsupported; header mới nhưng server thiếu XI2.4 cũng báo unsupported. Offline
suite kiểm tra riêng nhánh unsupported, mask **5 byte** bao phủ swipe end (32),
và decoder cancellation bằng dữ liệu giả; không chứng minh giao thức/hardware.

**Chỉ sau khi được chấp thuận riêng cho display/setup thử nghiệm** mới chạy các
lệnh sau (thay `:PRIVATE` bằng display cô lập đã xác nhận, không phải phiên đang
sử dụng). Đây không phải sự cho phép chạy probe trên phiên thật:

```sh
# Capability: chỉ query; không select events, không grab.
./test/xi2-gesture-probe --display :PRIVATE --mode capabilities
# Observation: chỉ sau khi được duyệt tương tác; giới hạn 1..60 giây (mặc định 10).
./test/xi2-gesture-probe --display :PRIVATE --mode observe --seconds 10
# Passive-grab: opt-in KHÁC, cần chấp thuận riêng về ảnh hưởng input.
./test/xi2-gesture-probe --display :PRIVATE --mode grab --allow-passive-grab --seconds 10
```

Không tự cài dependencies, đọc raw device, chạy daemon hay thử live input không
người giám sát. Server cô lập có thể kiểm tra protocol nhưng chỉ chứng minh swipe
vật lý nếu có đường đưa sự kiện touchpad thật vào setup được chấp thuận.

Log gồm version negotiated, device/use/attachment/enabled/name, class/source,
gesture max touches nếu có. `use`/`attachment` là giá trị XI device/master-slave.
Header, version XI2.4 và gesture class **không** chứng minh sự kiện thật đến WM.
Event log gồm begin/update/end, time, device/source, detail, delta và unaccelerated
delta, flags/cancelled, root/event/child và effective modifiers. Không diễn giải
`detail` hay dấu delta thành số ngón/hướng cho đến khi đã đối chiếu thao tác thật.

Observation trên root **không** chứng minh passive grab trên root hoạt động qua
cửa sổ ứng dụng, cũng không bảo đảm input ứng dụng được giữ nguyên. Grab thử từng
master pointer đang enabled trên default root, async cho cả hai thiết bị,
`owner_events=False`, `XIAnyModifier`; log request status, per-modifier status và
X errors. API **không có finger-count filter**: grab có thể chặn cả swipe không
phải ba ngón. Bỏ qua chúng ở classifier sau này không giải quyết interception.
Probe kết thúc theo deadline hoặc SIGINT/SIGTERM, ungrab/deselect rồi đóng kết nối;
đóng kết nối cũng giải phóng resource thuộc probe. Không có synchronous freeze
hay replay. Lỗi mất kết nối do Xlib kết thúc tiến trình; server thu hồi grabs.

### Evidence gate trước mọi feature implementation

Ghi riêng kết quả capability, observation và passive-grab; đừng suy diễn từ nhau:

- Chuỗi **vật lý** begin → updates → end cho cả bốn hướng, đối chiếu số ngón,
  `detail`, dấu/đơn vị delta, cancellation và IDs nguồn/master.
- Root delivery qua child application windows, modifiers giữ/thả, competing
  grabs, cancellation; ghi rõ master/root/setup và request/per-modifier status.
- So sánh hành vi ứng dụng khi probe tắt/bật: click, drag, scroll, keyboard,
  focus, swipe sai số ngón; grab không được nuốt input thông thường.
- Kiểm tra deadline, SIGINT/SIGTERM và input sau cleanup; lỗi grab có thu hồi
  toàn bộ grab của probe. Đây là runtime checks, không phải offline PASS.
- Không có private server/hardware setup được duyệt: ghi **SKIP/BLOCKED**, không
  tuyên bố đạt. Hiện physical delivery và input-preserving passive grab chưa xác minh.

Chỉ khi gate đạt mới xem xét classifier ba ngón (một action trên completed,
above-threshold, uncancelled swipe; reject short/ambiguous/wrong-finger), trái/tiếp,
phải/trước qua `view()`, lên/show và xuống/hide idempotent. Overview/keyboard toggle,
selection, bounded cached previews và regressions giữ focus/fullscreen/scratchpad/
restart đều còn hoãn; không map app ẩn để chụp, không finger-follow animation hay
cam kết thumbnail ẩn luôn live. Các lựa chọn kiến trúc chưa chốt cần duyệt riêng.

## Workspace overview

The overview uses a theme-colored card grid with rounded surfaces, a highlighted pending selection, workspace numbers/names and a current-workspace marker. Window previews are centered in their cards, with labeled fallbacks when unavailable. Window counts and overflow indicators keep crowded workspaces readable; the last row is centered and spacing follows the configured UI scale. A footer shows the keyboard controls. No additional rendering dependency is required.

`Super+o` opens/toggles a workspace grid on the pointer monitor. Arrow keys change only the pending selection; Enter or a workspace click commits it. A window-card click commits its workspace, then focuses the still-managed client after checking its XID and workspace. Escape cancels without changing the original workspace or focus. Opening/browsing never maps hidden clients or focuses applications. Show/hide are idempotent; external workspace changes, client lifecycle changes, reload and monitor geometry changes close the overlay.

The WM takes actual XComposite/XRender downscaled snapshots only when a compositor owns redirection and a client is viewable. Snapshots are also taken before workspace/monocle hiding and retained for hidden clients. The cache holds at most 64 opaque pixmaps, each at most 192×108 (about 5.1 MiB at 32 bits/pixel, excluding server overhead), with round-robin replacement and removal on unmanage. No application redirection ownership is changed. Without a compositor, or when capture fails, the overview remains usable as labeled cards without images; previews are cached, not live video. Large workspaces display as many cards as fit.

Actions: `overview_show`, `overview_hide`, `overview_toggle`. Configurations with explicit `bind` entries replace defaults, so an existing live config needs `bind = mod+o:overview_toggle`; this repository's example includes it. No installed configuration is changed automatically. With `swipe_enabled = 1`, three-finger up/down show/hide this UI and left/right move its pending selection without switching the physical workspace. `make check-overview` runs isolated controller/cache tests; real X rendering and compositor compatibility still need private-display visual verification.

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
- **Compositor** (`dani-comp`): soft 7-layer shadows, optional opening fade,
  inactive dim and application `_NET_WM_WINDOW_OPACITY`. It yields to an existing
  `_NET_WM_CM_Sn` owner. `compositor = 1` starts it at WM startup with shadows,
  no dim and WM-controlled motion (off by default). The `compositor` action
  toggles only a compatible dani-comp on this display, never a foreign owner or
  every dani-comp process. Standalone `dani-comp [--no-shadow] [--no-fade]
  [--dim 0.5..1] [--fade-ms 0..1000] [--reduce-motion]` keeps its defaults:
  fade 160 ms, dim 0.92. XPresent is optional, with copy fallback; hardware
  pacing is not guaranteed. Tests use private Xvfb displays.
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

## Opening and closing animation (Phases 1A/1B, opt-in)

```ini
compositor = 0
animations = 0
reduce_motion = 0
animation_open_ms = 140
animation_close_ms = 100
animation_workspace_ms = 0
animation_overview_ms = 0
```

Set `compositor = 1` and `animations = 1` to enable opening/remap fade-in and
close/withdraw fade-out with WM-managed dani-comp. Durations are independent
integers in **0..1000 ms**; invalid values are rejected with diagnostics.
`compositor = 0`, `animations = 0`, or `reduce_motion = 1` makes all WM-controlled
effects instant; duration `0` disables the corresponding effect.
Monocle/scratchpad hiding and return remain instant. Layout movement is
available as an explicitly opt-in **best-effort** compositor effect via
`animation_layout_ms`: the WM commits each final geometry once and dani-comp
moves an already-painted opaque, unshaped depth-24 picture with Render. Input is
not delayed or intercepted, so X can deliver a click/hover to the final geometry
while the picture is still between positions; this known visual/input mismatch is
accepted by the best-effort policy. Set it to `0` to keep layout changes instant.
Docks, fullscreen, direct mouse drag/resize, ARGB/translucent/shaped clients and
override-redirect windows stay instant; unsupported or stale pictures fall back
instantly. A drag that starts while a picture still moves shows that picture
until its movement ends. Focus, input routing, geometry state, workspace state and application
opacity properties are otherwise unchanged.

Workspace switches are instant by default. Set `animation_workspace_ms`
(integer **0..250 ms**, default 0) for an opt-in crossfade: dani-comp keeps the
already-painted images of the outgoing windows and fades them out above the new
workspace. The WM sends one PREPARE message before the switch and never waits for
the compositor. Input, focus and the real window state change at once, so a click
in the first part of the fade goes to the new workspace while the old image is
still the stronger one (at most about half the duration, so about 125 ms). The cap
of 250 ms keeps this short. Docks and override-redirect windows stay on top. The
switch stays instant when a fullscreen client is on the old or the new workspace,
when more than 8 windows leave, when the overview is open, and when anything is
not ready (dani-comp falls back instantly). An external pager move of one window
is also instant.

The overview is instant by default. Set `animation_overview_ms` (integer
**0..250 ms**, default 0) for an opt-in fade: dani-comp fades the overview in
and out, and the WM moves the selection highlight smoothly between cards. The
keyboard grab, Escape, commit and the selection work as before and are never
delayed. A click in the first part of the fade-in can already open a card that
is still faint, and a click during the fade-out goes to the window below. When
you open a card, the overview fades out over the new workspace. Menus and other
override-redirect windows stay instant. The highlight runs without a compositor
too; it needs `animations = 1` and `reduce_motion = 0`.

`reload_config` updates the running opted-in compositor without restarting it.
Changing motion settings snaps live fades to final opacity, removes closing
images, and affects subsequent events; re-enabling does not restart existing fades. Reload does not
start/stop a compositor: after changing `compositor`, use the explicit toggle
or next WM startup. The existing frame loop sleeps after fades finish (normal
shape/GC maintenance remains). Animation does not implicitly start a compositor.

Standalone CLI settings are **not** overridden by WM reload. To opt a manually
launched dani-comp into WM control, add `--wm-settings`; this replaces its fade
policy with valid WM settings and fails instant if the settings/WM disappear.
`--reduce-motion` is always a hard override. Keep `compositor = 1` when using
WM-controlled fades even if autostart starts the process. Do not run a second
compositor (including the example autostart's `fastcompmgr`) at the same time;
an existing owner is left alone. Without `--wm-settings`, existing standalone
fade defaults and flags remain authoritative. Standalone closing defaults to
**off** (`--close-ms 0`); use `--close-ms 100` to opt in on an unmanaged display.
Closing under a WM requires `--wm-settings` and compatible daniwm hide reasons;
standalone instances with a detected WM conservatively skip closing effects.

Close images transfer already-painted compositor pixmap/Picture references;
there is no capture request or wait in the WM. A missing, invalid, oversized, or
unpainted image closes instantly. At most **8 images / 32 MiB pixel payload** are
retained for at most **1 second**, with earlier cleanup on completion, remap,
workspace change, settings changes, errors, or compositor teardown. No hidden app
is mapped or queried to obtain a close image; images are never saved to disk.

Private versioned settings retain the five-word opening contract and add a
three-word closing property; ordered five-word hide notifications distinguish
WM hiding from close/withdraw. They are scoped to the supporting WM window,
not a capture/geometry protocol or a security boundary between clients sharing
the same X server. Deadlines reuse the compositor loop, not a WM timer/thread.
`make check-animation` covers
headless math/config checks; `make check-animation-xvfb check-animation-wm`
covers renderer, cancellation, pacing and WM reload checks on private displays.
`make check-layout-animation` covers best-effort layout movement on a private display.
`make check-workspace-animation` covers the workspace crossfade (copy and Present) on a private display.
`make check-overview-animation` covers the overview fade and highlight (copy and Present) on a private display.
No installed configuration is modified automatically.

## Config
`~/.config/daniwm/config` (`$XDG_CONFIG_HOME/daniwm/config` if set), `key = value`, `#` comment.
Missing file → defaults. Bad line → stderr + ignored, WM keeps running.

Scalars: `mod` (super|alt|ctrl), `border`, `border_focus/border_normal`,
`bar_bg/bar_fg/bar_acc/bar_dim` (hex, `#` optional), `bar_h`, `ws_w`,
`scale` (0.5–3.0, default 1.0 — HiDPI multiplier for WM chrome only:
`bar_h`/`ws_w`/`border`/`gap_outer`/`gap_inner`/float-step/drag-deadzone
plus bar font `size=`; fractional ok, e.g. `scale = 1.5`; reload applies live),
`bar_on/gaps_on` (1/true/yes/on), `compositor` (0 default, opt-in startup),
`animations` (0 default), `reduce_motion` (0 default), `animation_open_ms`
(integer 0..1000, default 140), `animation_close_ms` (integer 0..1000, default 100),
`animation_layout_ms` (integer 0..1000, default 0; best-effort movement duration),
`animation_workspace_ms` (integer 0..250, default 0; workspace crossfade duration),
`animation_overview_ms` (integer 0..250, default 0; overview fade and highlight duration).
Shadow/dim are standalone compositor flags,
not WM scalar keys; standalone fade defaults to 160 ms.
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
`bar_task_w` (0–512, default 0 = auto equal-share; e.g. `160` = buttons at
most 160px wide, left-aligned, clicks past them are no-ops; buttons shrink to
fit before any task is hidden),
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
`tray_size` (0 hoặc 8–64, default 0 = tự tính từ `bar_h`, tối đa 22 — cỡ icon tray), `tray_gap` (0–32, default 6 — khoảng cách giữa các icon tray, reload applies live).
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
dani-ai chat --continue [ten]  # mo lai chat cu (ID rieng; last = checkpoint moi nhat)
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

### Provider profiles va capabilities

Profile la opt-in; khong dung `--profile` thi key/base/model van resolve nhu cu.
Tao `~/.config/daniwm/ai.profiles.json` (nen chmod 600):

```json
{
  "version": 1,
  "profiles": {
    "openai-mini": {
      "base": "https://api.openai.com/v1",
      "model": "gpt-4o-mini",
      "key_env": "OPENAI_API_KEY",
      "capabilities": {
        "tools": true,
        "vision": true,
        "stream": true,
        "token_parameter": "max_completion_tokens"
      }
    }
  }
}
```

```sh
export OPENAI_API_KEY='...'
dani-ai --profile openai-mini --show-config
dani-ai --profile openai-mini chat
# File khac: --profile NAME --profiles-file /path/to/profiles.json
```

- Profile pin endpoint, model va bien moi truong chua key; khong fallback sang
  key env/ai.key khac. Khong luu key trong JSON. `--api-base`/`--api-key` bi
  tu choi khi dung profile; `--model` chi chap nhan neu trung model profile.
- Capabilities khai bao thu cong theo provider/model, khong tu probe:
  `tools:false` tat runner, `stream:false` chon nonstream; `vision:false`
  tu choi gui image ke ca anh trong session resume. Token parameter la
  `max_tokens`, `max_completion_tokens`, hoac `null` (khong ho tro flag
  `--max-tokens`). Ca stream/nonstream dung cung mapping.
- `/model` bi khoa trong profile; khoi dong lai voi profile khac de doi model.
  Session context co profile name/model/capabilities (khong co key); mismatch
  can `--trust-session`, nhung model van phai khop profile.
- API request bi rang buoc trong base profile; redirect khac origin van bi
  chan. Remote HTTP canh bao vi khong ma hoa key/conversation; nen dung HTTPS.
  File profile la cau hinh tin cay: nguoi sua file nay co the doi noi nhan key.
- Chua co auto-discovery hay doi profile giua REPL; retry/backoff la opt-in
  qua `--http-retries` (xem ben duoi). Capability
  khong dam bao provider that su tuan thu schema; loi API van duoc bao nhu cu.

### Session ID va nhieu phien cung luc

- Moi REPL/co che tool checkpoint dung `session-<uuid>.json` rieng. Resume
  tao nhanh ID moi, khong sua session nguon; `/new`, `/reset`, `/clear` va doi
  tool root cung bat dau ID moi. ID duoc in khi thoat, dung voi `--continue ID`.
- `last.json` la ban sao checkpoint vua duoc publish gan nhat, khong phai ID
  co dinh cua mot REPL. Khi nhieu phien chay, `--continue` co the mo phien khac;
  dung ID cu the de chon dung hoi thoai. Khong tu gop conversation.
- Ghi checkpoint/last va `/save` duoc serialize bang `flock` tren
  `.history.lock`; lock tu nha khi process thoat/crash. `/save` tu choi ten
  da ton tai thay vi ghi de. Readline merge chi cac entry moi cua moi REPL
  voi history tren dia, giu toi da 200 entry.
- Lock la advisory cho cac process dani-ai hop tac tren filesystem ho tro
  `flock` (Linux/local filesystem); khong bao ve truoc chuong trinh sua file
  ben ngoai. Hai file ID/last khong phai mot transaction crash-atomic:
  neu publish last loi, ban theo ID van con. Chua co retention/cleanup tu dong.

### Retry/edit-last an toan (REPL)

```text
/retry
/edit-last Prompt moi thay the toan bo prompt gan nhat
```

- `/retry` chay lai user turn gan nhat, bo cau tra loi cu; giu nguyen prompt
  va attachments. `/edit-last <text>` thay **toan bo** prompt, bo attachments
  cu, khong ho tro multiline/editor trong lenh nay. Cac luot truoc van giu.
- Tu choi neu turn do co bat ky tool call/result nao, ke ca read-only,
  failed/pending tool. Khong replay mutation/shell/custom, khong undo file.
  Hay gui prompt moi dua tren journal neu tool da chay. Tool execution cua
  request moi van theo permission/budget nhu binh thuong.
- Loi/huy request chua co tool: prompt bi rut khoi conversation nhu truoc,
  nhung giu RAM de `/retry` hoac `/edit-last` ngay sau do. Prompt loi nay
  khong persist qua restart; prompt moi hoac slash-command khac se bo cache.
- Truoc replay, checkpoint ban goc theo ID va tao nhanh moi voi prompt du
  kien; loi checkpoint khong doi conversation RAM hay goi provider. Loi API
  van giu prompt de thu lai, khong tu retry tool. Phien private chi thay RAM,
  khong co backup. Usage van cong don trong process, provider co the tinh phi
  cho moi lan request (ke ca request truoc do loi).
- `/retry` khong tham so; `/edit-last` can text khong rong. Khong chon turn
  tuy y, khong resume token stream, khong import prompt da mat sau restart.

### Session export/delete (local, khong can API key)

```sh
dani-ai history                                      # xem ten snapshot
dani-ai history export TEN json ~/chat-backup.json   # JSON giu metadata/messages
dani-ai history export TEN md ~/chat-backup.md       # Markdown de doc/luu tru
dani-ai history delete TEN CONFIRM                  # xoa dung snapshot TEN
```

- TEN la stem khong co `.json`, chi gom chu/so/`_`/`-`; co the dung `last`,
  ten `/save` hoac `session-<uuid>`. Sai cu phap/khong co `CONFIRM` khong xoa.
- Trong REPL: `/export json|md PATH` xuat **conversation hien tai trong RAM**,
  khong can `/save` hay goi model; PATH la phan con lai cua dong, ho tro space
  va `~` (khong them shell quotes). `/delete TEN` yeu cau nhap lai TEN de xac nhan.
- Export create-only, atomic publish, mode `0600`; parent phai ton tai va
  khong bi chmod. Tu choi ghi de file/symlink. JSON giu nguyen fields cua
  snapshot; Markdown giu message JSON trong indented blocks (gom tool calls,
  results va image payload), khong phai dinh dang import rieng.
- **Export chua du lieu rieng tu**: system prompt, context root/provider,
  toan bo tool output va attachments; khong scrub secrets. Khong in noi dung
  export ra terminal; user tu chon noi luu. Khong export stdout/overwrite.
- Delete chi unlink mot snapshot duoi history lock, tu choi symlink,
  hardlink/non-regular va JSON khong co messages list. Khong xoa alias/`last`
  theo session_id, exports, readline history hay workspace. Khong secure erase.
  Session dang chay van giu RAM va co the tao lai ID/last o checkpoint tiep;
  thoat cac phien truoc neu muon cleanup. Chua bulk delete/retention/undo-delete.
- `--no-history` chan export/delete, ke ca CLI local. Cac lenh CLI nay chay
  truoc provider/key/tool configuration, khong goi network hay model.

### Private mode va local history

- `dani-ai chat --no-history`: khong doc/ghi readline history, khong autosave,
  khong ghi tool checkpoint; `/save`, `/resume`, `/export`, `/delete` bi tat, khong dung chung
  `--continue`. Single-shot cung ton trong flag nay.
- Tool call/result van giu trong bo nho khi loi, nhung crash/kill se khong co
  journal tren dia de phuc hoi. Day KHONG phai che do offline hay sandbox:
  provider van nhan prompt, tool van co the ghi workspace theo permission policy,
  terminal/shell ben ngoai CLI van co the luu log.
- Slash-command (ke ca `/system`) khong them vao readline history moi; history
  cu khong duoc tu dong scrub. Session binh thuong van luu system prompt.
- `dani-ai history` la lenh local, khong can API key. Khi co `--no-history`,
  lenh khong doc session ma chi thong bao history da tat.

### Path/domain policy (builtin tools)

```sh
dani-ai chat --deny-path 'private' --deny-path '*backup*' \
  --write-path 'src' --write-path 'src/*' --allow-domain docs.python.org
```

- Cac flag lap lai duoc. `--deny-path GLOB` chan doc/ghi va an entry khoi
  list/find/search; ap dung ca khi `--include-ignored` hoac `--yes-tools`.
  Deny co uu tien cao hon write allowlist va blocklist/preset van giu nguyen.
- Glob la Python fnmatch case-sensitive tren path root-relative da normalize;
  `*` co the match ca `/`, khong phai gitignore. Ancestor bi deny thi ca cay
  con bi deny. Vi du `private` chan root/private va con, `*backup*` match
  path co chu backup. Khong nhan path tuyet doi hoac thanh phan `..`.
- Khi co `--write-path`, write/edit/mkdir chi duoc ghi path match mot glob.
  `make_directory` kiem tra tung parent vi co the tu tao parent; vi du cho
  phep `src` va `src/*` de tao thu muc con. Khong co flag thi giu policy cu.
- `--allow-domain HOST` la exact hostname (IDNA/case normalized), khong scheme,
  port hay wildcard; khong tu cho phep subdomain. Kiem tra truoc DNS o URL
  dau va MOI redirect. Khong co flag thi van cho phep public web theo SSRF checks.
- Preset `ask` hoi mot lan cho moi web origin (scheme/host/port) trong session,
  ke ca origin moi qua redirect. Tu choi thi dung fetch; approval xoa khi new,
  reset, clear, resume hoac doi root. `--yes-tools` bo prompt, khong vuot allowlist.
- Day KHONG phai sandbox/DLP: shell/custom process co the truy cap file/network
  ngoai policy. `git_status` la metadata cua repo, khong loc path theo deny;
  block bang `--block-tool git_status` neu can. Noi dung da co trong RAM/session
  khong duoc scrub khi them deny. Cac flag nay khong gioi han API provider endpoint.

### Diff/undo cua luot gan nhat

- `/diff` hien diff tong hop cac builtin `write_file`/`edit_file` trong luot
  user gan nhat, toi da 64000 ky tu hien thi; khong goi Git hay provider.
- `/undo` hien preview va hoi confirm (hoac auto-approve voi `--yes-tools`).
  Khoi phuc noi dung/mode cu, xoa file moi do AI tao; khong reset repo, khong
  xoa thu muc mkdir. Them thong bao vao conversation de model doc lai file.
- Snapshot chi trong RAM, budget 8 MiB/luot; vuot budget thi tu choi file
  mutation tiep theo. Luot user moi/new/reset/clear/resume/doi root xoa snapshot
  cu. Khong co undo qua restart hoac single-shot CLI; private mode khong persist.
- Undo preflight moi file: content va identity (inode/timestamps/size/mode/link)
  phai khop trang thai AI de lai, khong symlink/hardlink. User da sua file thi
  tu choi ca preflight, khong ghi de. Van tuan thu deny/write-path/read-only va
  tool blocklist. Neu phat hien edit chen giua hoac shell/custom chay trong
  luot thi tu choi undo vi khong theo doi day du side effects.
- Khong phai filesystem transaction/sandbox: publish undo tung file, loi I/O
  giua chung co the khoi phuc mot phan; conversation ghi ro cac file da restore.
  Check lai truoc publish nhung van co cua so race voi writer ben ngoai; tranh
  sua dong thoi luc undo. Khong cam ket bao ve truoc concurrent hostile writer.

### Scan cursor/pagination

- `find_files` / `search_files` tra `next_cursor`. Neu khac null, goi lai cung
  tool/path/pattern (hoac query) va them `cursor: "..."` de tiep tuc. Moi trang
  toi da 2000 entries scan, 100 matches, 16000 ky tu result; trang rong van co
  the co cursor neu scan nhieu entries khong match. `scanned/skipped/ignored`
  la counts cua trang hien tai, khong phai tong toan cay.
- Cursor opaque, single-use, process-only; toi da 4 scan dang do, idle TTL 120s
  (cleanup khi goi scan tiep). Scan thu 5 evict scan cu nhat. Reset/new/clear,
  resume/doi root va thoat REPL dong cursors. Param/root/deny/ignore thay doi
  thi cursor khong con hop le; bat dau scan moi. Khong persist fd/cursor.
- DFS giu directory/file descriptors no-follow de tiep tuc thay vi scan lai
  tu dau; toi da depth 32. `depth_limited` bao nhanh bi bo qua (khong the
  tiep tuc bang cursor); search bo file >64 KiB/binary. Dong match qua dai
  duoc clip va dem trong `clipped`, tranh mot dong chan tien trinh phan trang.
- Khong phai snapshot: filesystem thay doi giua trang co the thieu/thua/stale
  ket qua, directory dang mo co the da duoc rename. Thu tu theo filesystem,
  khong sort toan cay; quet lai neu can ket qua nhat quan sau khi sua file.

### Context va budget tool

- `/context` hien so message, tool result va do lon JSON UTF-8 cua hoi thoai.
  Day KHONG phai token usage/chi phi provider; khong tu compact.
- `/usage` hien `prompt_tokens`, `completion_tokens`, `total_tokens` do provider
  thuc su tra ve, tong cac response trong process hien tai va response gan nhat.
  Bao coverage tung truong; thieu counts la unavailable, khong coi la 0. Tool
  rounds va request dat ten `/save` cung duoc tinh. Khong uoc tinh token/cost,
  khong persist usage qua resume; request loi/stream do dang van co the bi tinh phi.
- Stream chi ghi nhan usage neu provider gui. `--stream-usage` opt-in gui
  `stream_options: {"include_usage": true}`; chi bat neu provider ho tro, khong
  tu retry/fallback khi server tu choi option. Nonstream khong can flag nay.
- `/compact [N]` giu N luot user gan nhat (mac dinh 4), giu system prompt va
  toan bo tool call/result trong cac luot do. Bo tron luot cu, KHONG tao summary
  va khong goi provider. Co the mat yeu cau/quyet dinh cu; chon N co chu dich.
  Binh thuong checkpoint ban goc theo ID, tao ID nhanh moi cho context gon;
  lenh in ID goc de resume. Neu checkpoint loi thi giu nguyen context trong RAM.
  Private mode chi rut gon trong RAM, khong co backup cac luot da bo.
- Moi luot user mac dinh toi da **64 tool calls** va **1 MiB tool-result UTF-8**;
  doi bang `--max-tool-calls N` / `--max-tool-output BYTES` (so duong).
  Batch vuot call budget se khong duoc chay. Output chi biet sau khi tool chay:
  neu vuot budget, ghi lai tool DA chay, dung cac call con lai va khong tu retry.
  Khong rollback file. Call/output budgets tach biet voi cooperative `--turn-timeout`.

### Turn deadline va retry opt-in

- `--turn-timeout SECONDS` (mac dinh 300, so duong huu han) la cooperative
  budget chung cho mot luot: cac API/tool rounds, retry sleeps va thoi gian
  cho approval. Request body, web va subprocess dung budget con lai; qua han
  thi khong bat dau tool/request tiep theo. Tool da chay khong rollback/replay.
- Day KHONG phai hard wall-clock deadline: DNS/connect/response headers,
  filesystem I/O, history lock va approval prompt co the giu process qua han;
  gate kiem tra khi control quay lai. Checkpoint van duoc ghi sau deadline de
  giu journal. Khong tu kill process giua filesystem mutation.
- `--http-retries N` (0..3, mac dinh **0**) chi retry API HTTP
  429/502/503/504 truoc khi body duoc doc. Backoff 1/2/4s hoac Retry-After
  (giay nguyen/HTTP-date); neu delay khong vua request/turn budget thi dung.
- Khong retry transport error, response body loi, JSON loi hay partial stream;
  khong retry tool/custom/web va khong replay mutation. Sau tool, chi request
  provider tiep theo co the retry voi cung context. Provider co the van tinh
  phi cho POST retry; bat flag la chap nhan rui ro duplicate provider work.

### Gioi han transport va test AI

- API JSON/stream toi da 16 MiB; moi dong SSE toi da 1 MiB, toi da 100.000 dong;
  noi dung answer toi da 2.097.152 ky tu. Request/session toi da 32 MiB;
  stdin toi da 2.097.152 ky tu. Vuot budget se bao loi, khong luu partial answer.
- `--timeout` la budget tinh tu luc bat dau request; vong doc response body
  giam socket timeout theo thoi gian con lai, chan server gui nho giot vo han.
  DNS resolver va pha ket noi/response headers cua urllib van phu thuoc timeout
  nen tang: day chua phai hard wall-clock deadline cho tat ca cac pha.
  Web fetch dung chung deadline qua cac redirect va body reads.
- `make check-ai` chay rieng, khong can X server hay API key that; `make check`
  cung bao gom suite nay. GitHub Actions co matrix Python 3.10/3.12/3.14.

### Tool use (bat mac dinh)

Tool builtin mac dinh: `list_files`, `read_file`, `search_text`, `find_files`,
`search_files`, `make_directory`, `write_file`, `edit_file`, `fetch_url`, `git_status`.
Chi can chay `dani-ai chat` trong thu muc project de yeu cau tao/sua file,
khong can flag shell. Mac dinh `--permissions ask`: hoi xac nhan truoc khi ghi,
fetch URL, chay shell hoac custom tool; thao tac doc file local khong hoi.

- `git_status()`: xem branch va thay doi cua repo chua tool root, khong can shell,
  khong ghi index/commit, bo qua submodule. Tool root mac dinh la thu muc luc
  khoi chay, KHONG mac dinh la thu muc cau hinh. Mo terminal tai project hoac
  dung `--tool-root ~/project` neu muon chon repo khac. Trong chat, go
  `/root ~/.config` hoac `/root ~/project` de doi root ngay, khong can shell;
  `/root` hien root hien tai. Duong dan tuong doi tinh tu root hien tai.
  Doi root se xoa hoi thoai de tranh dung noi dung file cu cho project moi;
  blocklist va quyen shell giu nguyen. Lenh nay do user go, model khong tu doi root.
- `read_file(path, offset=0, limit=16000)`: doc UTF-8 toi da 2 MiB/file.
  Offset/limit tinh bang ky tu; limit toi da 64.000. Neu `truncated=true`,
  doc tiep bang `next_offset`; khong cat noi dung am tham.
- `find_files(path, pattern)`: tim file de quy theo glob ten file, vi du `*.py`.
  Scan mac dinh bo qua `node_modules`, `vendor`, `build`, `dist`, `__pycache__`,
  `venv`, `coverage`, `*.o`, `*.pyc`. Them glob vao `.aiignore` tai tool root:
  moi dong mot pattern, dong `#` la comment; pattern khong `/` khop basename,
  pattern co `/` khop duong dan tu root; `/` cuoi duoc bo. Khong ho tro `!`
  negation hay day du gitignore semantics; KHONG tu doc `.gitignore`.
  `--include-ignored` bo qua cac ignore nay, nhung khong bo qua sensitive paths,
  symlink/hardlink restrictions. Ket qua tra them so entry `ignored`.
- `search_files(path, query)`: tim chuoi literal de quy, tra `path:line:text`.
  Moi trang scan toi da 2.000 entry, sau 32 cap, 100 ket qua/16.000 ky tu;
  bao `truncated`, `skipped`, `next_cursor` de tiep tuc. Search bo qua file
  >64 KiB, binary/khong phai UTF-8.
  Khong theo symlink, khong doc hardlink hay duong dan an/nhay cam.
- `make_directory(path)`: tao thu muc va cac cha con thieu, mode 700;
  chap nhan thu muc da co, tu choi symlink va duong dan nhay cam/ngoai root.
- `write_file(path, content)`: tao file moi, khong ghi de; thu muc cha phai co san.
- `edit_file(path, old_text, new_text)`: thay doan text khop chinh xac **mot lan**;
  model can doc file truoc. Khong khop/khop nhieu lan -> tu choi.
- `fetch_url(url)`: doc mot link cong khai http/https va tra text da trich xuat
  (kem title, status, mot so link). Timeout 20s, tai toi da 2 MiB, tra toi da
  16.000 ky tu; HTML duoc bo script/style/nav/footer. Tu choi scheme khac
  http(s), URL co credential, host private/loopback/link-local (SSRF guard,
  kiem tra ca redirect va IP thuc te), va file binary (anh/PDF...). Trang can
  JavaScript, sau paywall hoac chan bot co the khong doc duoc day du.
  Noi dung web la du lieu KHONG dang tin, khong phai instruction cho model.
  Chan bang `--block-tool fetch_url` neu khong muon model truy cap mang.
- Ghi/sua va search gioi han 64 KiB UTF-8/file; read_file toi da 2 MiB; chi trong tool root. Chan duong dan tuyet doi,
  `..`, symlink, hardlink (khi sua), file an/nhay cam, binary/device/FIFO.
- Ghi qua file tam va publish atomic, giu quyen rwx khi edit; file moi mode 600.
  write_file khong tu tao thu muc; dung make_directory truoc. Day khong phai sandbox cho shell/custom tool.
- Quyen tool: `--permissions ask` (mac dinh) hien diff truoc ghi file va hoi
  truoc mkdir/network/shell/custom tool. Khong co TTY thi tu choi prompt.
  `--permissions read-only` chi cho doc file/search/git status local; chan ca
  fetch URL, shell va custom tool. `--permissions workspace-write` giu hanh vi
  cu: ghi trong workspace va fetch URL khong hoi; custom tool van ton trong `confirm`.
  `--yes-tools` tu dong dong y prompt, KHONG vuot blocklist/read-only.
  Shell van can `--allow-shell`; day KHONG phai sandbox. Nen dung Git/backup.
- Session v2 luu project root va API base (khong luu key). Resume khac project,
  provider hoac session cu thieu metadata se bi tu choi; chi dung `--trust-session`
  sau khi da kiem tra noi dung va nguon goc. `/base` khong doi provider giua chat:
  khoi dong lai voi endpoint va key tuong ung de tranh gui nham key/history.
- REPL autosave sau moi cau tra loi; tool call/result duoc checkpoint vao
  `last.json` truoc/sau thuc thi, ke ca single-shot co tool. Luot loi sau tool
  duoc giu lai; file da sua KHONG tu rollback. Neu bi kill trong tool, ket qua
  luu co the ghi execution status unknown: kiem tra workspace truoc khi retry.
  Moi phien co ID rieng; `last.json` la latest snapshot dung chung, publish
  duoi lock (xem muc Session ID).

**`run_command` tat mac dinh**; chi bat bang `--allow-shell`.
Model tu quyet dinh khi nao goi tool; provider/model can ho tro OpenAI function calling.

**Canh bao:** sau khi bat `--allow-shell`, `run_command` chay `/bin/sh -c`
voi quyen user hien tai; `ask` hoi xac nhan tung lenh, con
`workspace-write`/`--yes-tools` cho phep chay khong hoi. Co the ghi/xoa file, truy cap mang va doc duong dan ngoai root.
`--tool-root` chi la working directory cua shell, KHONG phai sandbox.
Chi su dung voi model/provider va noi dung ban tin tuong. Chan `run_command`
neu muon chi doc file. Blocklist theo ten tool khong phai bo loc lenh shell.

```sh
dani-ai chat --tool-root ~/project
dani-ai chat --allow-shell         # opt-in shell KHONG sandbox
dani-ai "Doc README.md va tom tat"
dani-ai "Tom tat noi dung https://example.com/bai-viet"
dani-ai chat --block-tool fetch_url   # khong cho model mo link
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
Trong chat co the go `/shell on` de bat shell cho phien hien tai (vi du de
yeu cau Git commit/push), `/shell off` de tat; `/shell` xem trang thai.
Day la quyen shell DAY DU, khong chi Git; model co the ghi file, dung mang,
truy cap ngoai tool root. Chi bat khi tin tuong model va noi dung hoi thoai.
Blocklist van duoc ton trong; lua chon khong tu luu sang phien moi.

Root mac dinh la cwd. Cac tool file (khong bao gom shell) khong cho duong dan tuyet doi, `..`, symlink,
file an, mot so ten secret/key pho bien; khong doc hardlink hay device/FIFO.
read_file doc toi da 2 MiB/file, tra tung trang 16.000 ky tu mac dinh;
search/ghi/sua toi da 64 KiB/file; toi da 32 vong tool.
Custom tool giu nguyen output trong gioi han `output_limit` cua manifest.
Response co `finish_reason: length` hoac `content_filter` duoc bao loi,
khong luu cau tra loi do dang vao history (stream co the da hien text mot phan).
`search_text` tim chuoi literal trong mot file, khong de quy.
Ket qua file duoc gui den API provider: chi dung root chua du lieu ban muon
chia se. Bo loc ten secret khong the nhan dien moi du lieu nhay cam.
Neu can cam doc noi dung, chan `run_command`, `read_file`, `search_text`,
`search_files` va `edit_file` (edit doc file de tim doan khop). Chan ca custom tool co quyen doc.

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

Ngoai cac tool builtin, dani-ai nap them manifest JSON trong
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

BTC tool gioi han 2 MiB/response, budget doc du lieu 24s trong process timeout
30s; tra `partial` va `errors` neu mot section loi/het budget, giu section da lay.
Timestamp dung unix milliseconds; ticker/funding/OI co `age_seconds` khi nguon
tra timestamp. Nen co `close_time`/`is_closed`; nen cuoi co the chua dong.
`annualized_pct` chi la phep ngoai suy gia dinh funding 8 gio va rate khong doi,
KHONG phai lich funding da xac minh, du bao hay loi nhuan.
DNS/connect/header van theo gioi han timeout nen tang.

BTC multi-timeframe va OI history la opt-in:

- `intervals`: 1..4 khung duy nhat, phai chua `interval` chinh (mac dinh `1h`);
  tong `len(intervals) * limit <= 400`. Chi klines lay nhieu khung; funding,
  OI va long/short van lay mot lan, theo interval chinh khi can period.
- Khi co nhieu khung, `timeframes[interval]` chua candles, `window` (gom nen
  chua dong) va `closed_window` (chi nen da dong). `klines`/`window` top-level
  van la khung chinh de tuong thich. Loi tung khung ghi `errors[].interval`,
  giu cac khung thanh cong; moi request dung chung budget 24s, khong retry.
- Them `open_interest_history` vao `include` de goi `openInterestHist`;
  mac dinh van chi co nam section cu. Period mapping giong long/short.
  Binance chi cung cap lich su OI trong thang gan nhat. Tra samples timestamp,
  contracts/value, age va % thay doi dau-cuoi; duoi hai mau hoac mau dau bang
  zero thi change la null. Mau OI loi/non-finite/am hoac timestamp khong tang
  lam section loi thay vi am tham suy dien xu huong.
- OI snapshot khong the ket luan OI trend. OI value con phu thuoc gia;
  % thay doi chi mo ta cua so mau, khong la tin hieu mua/ban, BOS/CHoCH hay
  cam ket loi nhuan. Chua tu dong phoi hop price/OI hoac phat hien SMC.
- Manifest mau tang output budget len 256 KiB de chua nhieu khung;
  copy lai ca `.py` va `.json` neu da cai tool cu.

Vi du tool arguments:

```json
{"interval":"1h","intervals":["4h","1h","15m"],"limit":100,"include":["klines","open_interest_history"]}
```

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
  Single-shot khong sua session nguon; neu co tool se checkpoint theo ID moi.
  REPL checkpoint moi luot va khi thoat.
- Terminal hien spinner `Đang chờ phản hồi…` cho den khi co noi dung tra loi
  (ca stream va non-stream); tu don dep khi loi/Ctrl-C. Pipe khong co spinner.
- Loi API/timeout hoac Ctrl-C khi dang request khong dong REPL. Luot loi
  chua chay tool bi loai khoi context; neu da co tool thi giu journal va can
  kiem tra workspace truoc khi thu lai. Cac luot thanh cong duoc autosave.
- Stream phai ket thuc bang `[DONE]`. JSON loi, error event hoac stream bi cat
  duoc bao loi; phan text da hien thi khong duoc luu nhu cau tra loi hoan chinh.
  Token duoc hien thi ngay, khong doi newline; mau inline duoc render theo chunk.
- History va readline history duoc ghi atomic: thu muc `0700`, file `0600`.
  Day van la plaintext, khong phai ma hoa. File session cu chi duoc thay doi
  quyen khi ghi lai.
- Redirect khac origin (scheme/host/port), ke ca HTTPS sang HTTP, bi chan de
  tranh gui API key sang endpoint khac. HTTP van duoc ho tro cho local server;
  hay dung HTTPS cho endpoint remote.
- `fetch_url` chi GET, khong gui API key hay du lieu nguoi dung. Host duoc
  resolve va chan neu la private/loopback/link-local/reserved; redirect duoc
  kiem tra lai tung buoc (toi da 5), HTTPS -> HTTP bi tu choi. Gioi han 2 MiB
  va 20s moi request. Khong co JavaScript, cookie hay dang nhap, nen trang
  dong bang JS co the tra ve rong hoac thieu noi dung.
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
