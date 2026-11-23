# Phase 3-I — workspace crossfade integration

## Inspect/plan statement (written before code)

Entry: the user asked for 3-I on 2026-11-23. The 3-P1 and 3-P2 gates passed
after review. The 3-P2 review capped `animation_workspace_ms` at 0..250 ms,
default 0 (opt-in). Phase 2 strict stays BLOCKED.

Design: dani-comp reuses its Phase 1B exit pool for the outgoing images. It
does not add a second renderer. The pool already gives one shared budget
(8 images, 32 MiB), a TTL, shape clips, dim, rank order, drawing above normal
windows and below docks/override-redirect/fullscreen, Present/copy, and the
async X error path.

WM (`src/client.c`, `src/keys.c`, `src/monitor.c`, `src/config.c`):

1. New setting `animation_workspace_ms`: integer 0..250, default 0.
2. New root property `_DANIWM_ANIMATION_WORKSPACE_SETTINGS_V1`: version,
   owner, duration. The five-word Phase 1A contract is unchanged.
3. New root property `_DANIWM_MONITOR_EPOCH_V1`. The WM bumps it when the
   monitor geometry changes.
4. `view()` and `send_to()` call a new `animation_workspace_prepare()` before
   `ewmh_desktops()`. It sets `_DANIWM_WORKSPACE_V1` on root and sends a
   PREPARE ClientMessage with the same sequence. It never reads from or waits
   for the compositor.
5. Members are the viewable outgoing clients only (`c->ws == old`,
   `!c->hidden`). At most `EXIT_COUNT` (8), the size of the shared pool.
6. Policy: no PREPARE when animation is off, when reduced motion is on, when
   the duration is 0, when the outgoing or incoming workspace has a
   fullscreen client, or when there are more than 8 members. External
   pager moves (`move_to()`), scratchpad and reload send no PREPARE.
   (Added during the work: no PREPARE when the overview is open or commits.)
7. `animation_unmap()` also sends hide reasons when only the workspace
   duration is on (today it needs open or close > 0).

dani-comp (`src/comp.c`, `src/exit-layer.h`):

1. Read the duration only with `--wm-settings` and a valid owner. It is off
   when fades are off or reduced motion is on.
2. On PREPARE: validate version, owner, monitor epoch, sequence, payload
   length and age (≤ 100 ms). Clear all exit layers (newest wins).
3. At each member UnmapNotify with a WORKSPACE hide reason: take its
   already-painted pixmap and Picture into a pending scene. Any member that
   is not eligible ends the scene (bypass).
4. When every member is kept and the desktop property equals the target,
   admit all images into the exit pool at once, with one start time. If the
   pool cannot take them all, free them (bypass).
5. The "painted" mark and the pixmap size check also run when only the
   workspace duration is on (today they need close > 0).
6. Workspace layers have their own kind. Workspace and monocle hide reasons
   clear only close layers. Reload, a desktop change without PREPARE,
   monitor epoch, WM owner, settings, root resize and compositor exit clear
   everything.

Tests:

- Headless: `animation_workspace_ms` parsing (0..250) in
  `test/test-config-animation.c`.
- New `make check-workspace-animation`: real daniwm with WM-managed dani-comp
  on a private Xvfb. It checks the early and final frames, the input target
  during the fade, a monocle workspace, the fullscreen policy, duration 0,
  reduced motion, a switch storm, a monitor epoch bump and the XRes baseline.
- The new test also covers: open = close = 0 with workspace = 250; a switch
  right after the overview opens and closes; a close just before a switch
  (its close image must not follow); Present on and off.
- All existing animation, compositor and overview suites, and `run-all.sh`.
  `check-animation-wm` must pass unchanged (workspace default 0).

Docs: README and the example `config` get the setting, the 0..250 ms cap,
default 0, and the input/visual mismatch (at most about 125 ms).

## Results

All checks pass on a private Xvfb. `make check-workspace-animation` passed 3
runs in a row after the last change (each run does the copy path and the
Present path). Copy numbers are first, Present numbers are second.

| # | Check | Result |
| --- | --- | --- |
| 1 | `view()` sends one PREPARE with one member | PASS |
| 1 | Crossfade 0→1: blend frames, then the exact incoming frame | PASS (settle 250 / 269 ms) |
| 1 | Early frame: the outgoing image dominates | PASS (`c60006` at 13 ms / `bd000f` at 42 ms) |
| 2 | Real XTest click during the fade goes to the incoming client | PASS (incoming 1, outgoing 0, focus incoming) |
| 3 | Monitor epoch notification mid-fade: exact final frame at once | PASS (blend at 10 / 26 ms, final 40 ms later) |
| 4 | Monocle: PREPARE lists only the shown client; the hidden client never appears | PASS |
| 5 | `send_to()`: the moved client is not a member; crossfade, exact final frame | PASS |
| 6 | Fullscreen on the incoming or the outgoing workspace: no PREPARE, instant | PASS (settle 4 / 12 ms) |
| 7 | `animation_workspace_ms = 0`: no PREPARE, instant | PASS |
| 7 | `reduce_motion = 1`: no PREPARE, instant | PASS |
| 8 | Close just before a switch: the close image does not follow | PASS (green after the first switched frame: 0) |
| 9 | Switch with the overview open: no PREPARE, exact final frame at once | PASS |
| 9 | Commit from the overview (real Left + Return keys): no PREPARE, exact final frame | PASS |
| 9 | Switch right after the overview closes: exact final frame | PASS (bypass, see Findings 2) |
| 10 | Storm, 10 switches 25 ms apart: 10 PREPAREs, newest wins, exact final frame | PASS (settle 232 / 241 ms) |
| 11 | Shown scratchpad is a member (2 members): crossfade, exact final frame | PASS (settle 252 / 277 ms) |
| 11 | Scratchpad hide during a fade: the fade continues, exact final frame | PASS (settle 231–246 ms after the hide) |
| 11 | 3 tiled members: each tile crossfades, then the exact final frame | PASS (settle 252–261 / 263–284 ms) |
| 12 | 9 leaving clients: no PREPARE, instant | PASS |
| 13 | dani-comp XRes after all of the above | PASS (36/36 and 38/38 resources; pixmap bytes equal) |
| — | No unexpected `dani-comp: Xerror` line in the log | PASS (both paths) |
| — | Headless: setting 0..250 and PREPARE validation | PASS (`make check-animation`) |

XRes note: `CompositeClientWindow` goes from 7 to 8 because root has one more
child window at the end (7 → 8, created by the WM). The test subtracts the
number of root children. This is the same note as in 3-P2.

Existing suites after the last change: `check-animation`,
`check-animation-xvfb`, `check-animation-wm` (unchanged, workspace default 0),
`check-layout-animation`, `check-layout-prototype`, `check-overview`,
`check-ws-capture`, `check-ws-render`, `test/test-comp.sh`,
`test/test-comp-present.sh` and `test/run-all.sh` pass. In the last
`run-all.sh`, `test-urgency` (`HINT_SET_OK`) failed one time (267 PASS). That
test is flaky before this change too: 1 of 10 runs failed on the committed
HEAD, and 0 of 10 failed with this change.
dani-comp also builds with no warnings without XPresent.

`check-ws-render` failed one time when it ran right after the other suites
(an unexpected BadRegion in the test-only model). It passed two runs in a
row alone. That test does not use `src/`: `test/test-ws-render.sh` does not
name `daniwm`, `dani-comp` or `src/`, and `test/ws-render-xvfb.c` includes
only `test/ws-capture-prototype.h` from this repository.

Bypass count (6 extra runs with `DANI_COMP_DEBUG=1`, both paths): the
3-member switch never bypassed. Each copy run had 1 bypass (Findings 2).
Each Present run had 2 (Findings 2 and 3).

Mutation check for the scratchpad hide row: with dani-comp changed so that a
scratchpad hide clears everything, that row fails (the fade settles 3–33 ms
after the hide). `src/` was restored after the check (`git diff` empty), and
the real code then passed 3 runs in a row.

## Changes from the plan statement

1. **Overview.** `view()` and `send_to()` send no PREPARE when the overview is
   open or when the overview commits the switch (new
   `overview_committing()`). The old workspace is not on screen then, so a
   crossfade from it would show an image the user did not see.
2. **Scratchpad hide.** Like workspace and monocle hides, it clears only close
   images. Only reload clears everything.
3. **Debug log.** With `DANI_COMP_DEBUG`, dani-comp prints the reason for each
   workspace bypass.

## Findings

1. **The crossfade works on both paths.** Copy shows the first blend at about
   13 ms. Present shows it at about 42 ms. Assumption, not measured: Present
   on Xvfb paces frames, so the first small alpha change shows later.
2. **The paint pre-pass reads live server state ahead of the event queue.**
   When the stack changed, `repaint()` asks the server for each window's map
   state (`XGetWindowAttributes` in the `stack_dirty` pre-pass). If an earlier
   event set `stack_dirty` and a paint runs before the PREPARE is read, the
   server can already show the members unmapped. dani-comp then marks them
   unmapped, and PREPARE bypasses. The switch is instant and exact. The test
   shows it when the overview closes and a switch follows at once (the
   overlay destroy sets `stack_dirty`). It did not happen in the 3-member
   switch (0 of 12 runs).
3. **A window resized just before a switch bypasses on Present.** In the
   close-then-switch case, the close makes the remaining client wider. Its
   picture is freed on resize. On Present, it was not painted again in the
   15 ms before the switch, so `painted` was 0 and the scene bypassed. A
   picture of the old size would be wrong, so this is correct. The close
   image still does not follow.
4. **Input/visual mismatch is as accepted in 3-P2.** In the Present run, the
   click at 20 ms showed the pure outgoing colour while input went to the
   incoming client. The 250 ms cap keeps this short.

## SKIP and open items

- The monitor epoch check rewrites the property from the test. On Xvfb,
  `XRRSetMonitor` sends no RandR event (3-P2 Finding 5), so the WM bump in
  `on_monitors_changed()` is not run by this test. It is the same call path
  as for any monitor change. Not measured.
- ARGB and shaped members are not tested here. They use the same image path
  as close (3-P2 covered them in the model).
- Hardware pacing, tearing and real input latency: SKIP (Xvfb).

## Decision: gate PASS (reviewed 2026-11-23)

The user reviewed this report and accepted:

1. No crossfade when the overview is open or commits the switch.
2. A scratchpad hide clears only close images; a running workspace fade
   continues. Only reload clears everything.

Tearing, vsync pacing and real input latency are still not verified (Xvfb
only). Check them on real hardware before release.

## Manual check in Xephyr (2026-11-23)

The user tested the built binaries by hand in an isolated Xephyr (`:5`,
1600×900) with real keyboard and mouse. The live session was not changed.

- Config: the user's config with `mod = alt`, `compositor = 1`,
  `animations = 1`, open 140, close 100, layout 160, workspace 180 ms.
  `HOME` and `XDG_CONFIG_HOME` pointed to a temporary folder, and
  `dbus-update-activation-environment` and `systemctl` were stubbed.
- User result: "mượt, không lỗi" (smooth, no errors).
- Evidence: the WM sent 9 PREPAREs (last sequence 9). The WM/compositor log
  has no `Xerror` line and no crash.
- Limit: Xephyr draws in software with no GPU vsync. This check covers feel
  and correctness, not tearing or frame pacing. A check in the real session
  is still open.
