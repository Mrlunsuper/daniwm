# daniwm audit — 1870-line single-file X11 tiling WM

**Date:** 2026-09-09
**Scope:** full read of `daniwm.c`, `PLAN.md`, `Makefile`, `README.md`, `test/*.sh`; clean rebuild; headless runs under Xvfb.
**Verdict: solid. Build clean (`-Wall -Wextra`, gcc 16), all suites pass. No crash, hang, or zombie found. Remaining items are EWMH gaps, one layering bug, and hardening notes — no rewrite needed.**

## Verification results

* `make clean && make` — 0 warnings, exit 0.
* `test/verify.sh` — 18/18 PASS (Xvfb 1280x800):
  * 3 tiled windows visible; master `14,38,681x744`; stack `x=707`
  * monocle `10,34,1256x752` (1 visible)
  * gaps-off removes outer gap, gaps-on restores
  * ws2 empty, back to ws1 with 3, move-follow ws2=1 / ws1=2
  * fullscreen `0,0,1280x800`, scratchpad spawn/hide/reshow
* `test/test-config.sh` — 4/4 PASS (custom binds survive bad lines, `gaps_on=0`+`mfact=0.8` → master `x=0,w=1020`, reload restores `x=14`)
* `test/test-kill.sh` — 7/7 PASS (Super+q kills one/last, WM survives, respawn works)
* `test/test-mouse.sh` — 4/4 PASS (move +120+80, resize +60+40, dragged floats in monocle, deadzone click stays tiled)
* `test/test-strut.sh` — 8/8 PASS (workarea `0,0,1280,800` → `0,40,1280,760` with top dock, xterm `y=54`, dock survives ws switch + Super+q, `y=14` + workarea restore after kill)
* `test/test-workspaces.sh` — all PASS (move-follow, `mod=alt` rebuild, shrink `workspaces 3→2` folds ws3 into ws2)

## Strengths

* Careful ICCM/EWMH patterns: synthetic-`UnmapNotify` (`send_event`) withdraw vs. `XUnmapWindow` for view switching; `SubstructureRedirect` single-WM guard via `xerror_other_wm`.
* No zombies: `SIG_IGN` for `SIGCHLD`, `spawn()` closes X fd + `setsid`.
* Safe config exec: `wordexp(WRDE_NOCMD)` + `execvp` (no shell), autostart via `execl` only if `X_OK`, fixed-string `popen(amixer)`, all `snprintf`.
* OOM-safe `finalize_nws()` (keeps old set on malloc fail), per-ws `mfact/nmaster` preserved across reload, extra ws folded into last, parked scratchpad sentinel follows `NWS`.
* Tiled `ConfigureRequest` correctly drops move/resize (only `CWSibling|CWStackMode` passes through); floating geometry tracked (`fx/fy/fw/fh`); fullscreen saves/restores floating geometry.
* Key grabs handle `LockMask|Mod2Mask`; state match masks out Num/CapsLock — correct.
* Docks never focused/killed (Enter/Button/kill paths exclude `find_dock`).

## Findings

### High — fix or document

1. **Incomplete EWMH workspaces.**
   Publishes `_NET_SUPPORTED/CLIENT_LIST/ACTIVE_WINDOW/WM_STATE/WINDOW_TYPE/CLOSE/STRUT/WORKAREA` but not `_NET_NUMBER_OF_DESKTOPS`, `_NET_CURRENT_DESKTOP`, `_NET_WM_DESKTOP`, `_NET_WM_STATE_HIDDEN`.
   Effect: pagers/rofi/wmctrl see one desktop; taskbars never hide off-ws windows.
   Fix: publish number/current (~20 lines, update on `view`/`send_to`/`finalize_nws`), or scope README to "EWMH: fullscreen+dialog+dock+struts only".

2. **`focus()` raises above docks (layering bug).**
   `arrange()` correctly raises docks then fullscreen, but `focus()` — called right after in `manage/view/send_to/drag_end/MapRequest` — does unconditional `XRaiseWindow(c->win)`.
   Focused tiled client ends up over Polybar/Tint2.
   Fix: don't raise in `focus()` for non-floating, or re-raise docks after.

3. **`sys_vol()` blocks the event loop.**
   `drawbar()` runs on every `arrange`/`focus` + 1s tick; `popen("amixer …")` forks+execs synchronously (2s cache). Missing `amixer` still forks `sh+grep+head` every tick — the only jank source.
   Fix: sample volume on tick only (not per arrange), negative-cache longer when `amixer` absent, or make sysmon async.

### Medium

4. **No monitor hotplug.** `initmons()` (Xinerama) runs once at startup; RandR changes need restart. Fine for minimal — document it.
5. **Strut math assumes one rectangle.** Bottom/right use global `sw/sh` (`b_top = sh-b`, `r_left = sw-r`). Correct for side-by-side and full-width stacked, wrong for uneven/negative-offset Xinerama. Use per-monitor geometry + partial ranges only.
6. **Bar text geometry hardcoded.** `XDrawString(…, y=16)` regardless of `bar_h 8..64`; `bar_h≠24` misaligns. Center with `font->ascent/descent`. Related: X core fonts only (`fixed`/`9x15`), `XFetchName` Latin-1 — CJK/emoji garble. Xft is the real fix, out of scope for minimal.
7. **Iconify loses state.** Synthetic unmap → `unmanage`, remap → fresh `manage` (ws/float reset). Standard dwm tradeoff — note it.
8. **Cross-monitor drag promotes to float.** `drag_end` sets `mon` by release point but a tiled window crossing monitors stays floating instead of re-tiling on the new monitor. Users expect move-to-monitor.

### Low / nits (correct but fragile)

* `parse_bind` `strcpy(combo,val)` + double `strtok` works (same-length dup, `sep-val` offset) but is a maintenance trap; simplify/comment.
* `NMASTER` uncapped (`k_nmasterinc` no max) — harmless (`min(nmaster,n)`); cap at 8 like config.
* `getarea` clamps to 50px when struts+bar exceed monitor — overlaps panel; intentional, keep.
* Parked scratchpad (`ws==NWS` sentinel) is handled consistently but stays in `_NET_CLIENT_LIST` while hidden — pagers show a phantom. Filter or set hidden state.
* Custom `scratch =` without `scratchpad` in WM_CLASS/NAME respawns forever on toggle. Document the `-name scratchpad` contract in README config section (currently only in PLAN).
* `select()` loop: `ret<0 && !=EINTR` falls through to blocking `XNextEvent`; add a log for debuggability.
* Static scan: no `strcpy/strcat/sprintf/gets` on untrusted input; `malloc`s checked except trivial `xstrdup` wrappers whose callers tolerate NULL (wildcard rules). `XGetWindowProperty` strut cast checks `format==32 && n==12/4` — OK on LP64.

## Security notes — good

No `system()`, no shell interpolation of config, no setuid, no network. Attack surface is X clients + config file + autostart script, all handled with standard precautions above.

## Round 2 (2026-09-10) — all requested, all green

5. ~~Xft/UTF-8 bar.~~ DONE: Xft replaces X core fonts (`-lXft` via pkg-config); `font` is now a fontconfig pattern (default `monospace:size=10`); titles via `_NET_WM_NAME`+fallback with UTF-8-safe truncation; per-glyph fallback across `Noto Sans`/`DejaVu Sans`/`Sans` so base-font gaps (e.g. `✓` in monospace, `ế` in DejaVu Mono) still render. Verified: `Tiếng Việt ✓ nhạc` screenshot fully rendered; all suites PASS.
6. ~~Volume backend fallback.~~ DONE: sample tries `amixer` → `wpctl` → `pactl`, first success sticks (single probe per tick after); vol keys/bar scroll/mute use the matching setter (`5%+`/`toggle` per backend). Verified end-to-end with fake failing `amixer` + fake `wpctl 0.42` → bar shows `V 42%`.
7. ~~Strut math for uneven/negative-offset layouts.~~ DONE: new `screen_extents()` (monitor bounding box, `sw/sh` fallback pre-`initmons` + explicit `update_struts()` after it — this ordering caused one `test-strut` failure mid-work, fixed); all four edges + `_NET_WORKAREA` origin-aware. `test-strut` 8/8.
8. ~~Cross-monitor drag re-tiles.~~ DONE: `Drag` records `tiled0/mon0`; a tiled window moved (Mod+Left) to another monitor re-tiles there, same-monitor drops still promote to float, resize-drags always stay floating. `test-mouse` still PASS (same-monitor case).

## Round 1 — suggested order (all DONE)

1. ~~Add `_NET_NUMBER/CURRENT/WM_DESKTOP` — biggest compat win.~~ DONE (2026-09-10): root `NUMBER+CURRENT+NAMES`, per-client `WM_DESKTOP` (parked scratchpad → sticky `0xFFFFFFFF`), pager requests honored (`_NET_CURRENT_DESKTOP` → `view`, `_NET_WM_DESKTOP` → `move_to` without follow), initial hint honored on manage. Verified with `xprop` + `xdotool set_desktop_for_window`.
2. ~~Fix focus-over-dock raise.~~ DONE (2026-09-10): new `keep_docks_on_top()` helper (docks, then fullscreen above all); `focus()` only raises floating/fullscreen and re-raises docks+fullscreen after — tiled focus does zero restacks (also less flicker on focus-follows-mouse). Same guard added to float-promote, unfloat-raise, and mouse-drag promotion. Verified live under Xvfb: `XQueryTree` bottom→top stays `… xterm, dock` before and after `windowactivate`+`super+j`; all suites still PASS.
3. ~~Throttle `sys_vol()` to tick-only + negative cache.~~ DONE (2026-09-10): split into non-blocking `sys_vol()` (cache read) + `sys_vol_update()` (blocking `amixer`, 1s tick only). arrange/focus storms no longer fork. amixer-absent backoff 8s → 30s.
4. ~~Center bar text from font metrics; document restart-for-hotplug + scratchpad naming.~~ DONE (2026-09-10): baseline `= (BAR_H + ascent - descent)/2` clamped, all 5 bar strings — verified `bar_h = 40` screenshot (centered, workarea y=40). README: Xinerama read-once → restart note; custom `scratch =` must yield `scratchpad` in class/name.
