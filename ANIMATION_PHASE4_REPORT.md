# Phase 4 — overview polish

## Inspect/plan statement (written before code)

Entry: the user asked for Phase 4 on 2026-11-23, after the 3-I gate passed.
The plan has no prototype subgate for Phase 4.

Today the overview is an override-redirect window that covers one monitor
(`src/overview.c:278`). It grabs the keyboard and draws everything with core
X into a backing pixmap. dani-comp shows override-redirect windows at once:
no fade in (`opening_start`, `win_add`, `win_alpha`) and no exit image
(`closing_detach`, `exit_layer_from`).

User decisions (2026-11-23):

- Scope: **4A and 4B** in this request. 4A first, tested, then 4B.
- `animation_overview_ms`: integer **0..250, default 0** (opt-in), like
  3-I. One setting for both parts.
- Commit from the overview: the overview image **fades out over the new
  workspace**. Input and focus belong to the new workspace at once.

Scope:

- **4A: overview opacity fade in and fade out.** All animation is in
  dani-comp. The WM only marks the overview window.
- **4B: selection highlight animation.** The WM loop has only a 1 s clock
  tick and a volume deadline (`src/main.c:490`). 4B adds an overview frame
  deadline to that loop and redraws the overview while the highlight moves.
- Scale effects stay out of scope.

WM (`src/keys.c`, `src/config.c`, `src/overview.c`):

1. New setting `animation_overview_ms`: integer 0..250, default 0.
2. New root property `_DANIWM_ANIMATION_OVERVIEW_SETTINGS_V1`: version,
   owner, duration.
3. New root property `_DANIWM_OVERVIEW_V1`: version, owner, overlay XID. The
   WM writes it before `XMapRaised` and deletes it on hide. The X calls go
   in `keys.c`, because `test/test-overview.c` stubs the X calls of
   `overview.c`.
4. No change to open, browse, commit or cancel. Hide still destroys the
   overlay and releases the keyboard grab at once.

dani-comp (`src/comp.c`, `src/exit-layer.h`):

1. Read the duration only with `--wm-settings` and a valid owner. Off when
   fades are off or reduced motion is on.
2. At MapNotify, mark the window as the overview only if its XID equals the
   published overlay. Bind the mark to the window generation. Menus,
   dani-run, the tray and other override-redirect windows stay instant.
3. Fade in: the overview window gets an opening fade with the overview
   duration. It is not an opaque occluder while it fades, so the workspace
   below is still drawn.
4. Fade out: at its UnmapNotify, keep its already-painted image as an exit
   layer of a new kind. Draw it on top of everything (also above the bar),
   because the overview was on top. It is drawn by dani-comp only, so it
   takes no input.
5. A new overview map drops any old overview image (no stacking).
6. Clearing: hides and the desktop change keep the overview image (so it
   fades out over the new workspace after a commit). PREPARE, reload,
   owner, settings, root resize, monitor epoch and compositor exit clear
   it.
7. Widen the `painted` and pixmap-size gates to include the overview
   duration (the same hidden gate as in 3-I).
8. Budget: a full-monitor image uses the shared 32 MiB pool. At
   3840×2160 it is 33,177,600 bytes, so it fits only when the pool is empty.
   If it does not fit, the hide is instant.

4B, WM only (`src/overview.c`, `src/main.c`):

1. When the selection moves, the old card fades from the selected colours
   to the normal colours and the new card fades the other way, over the
   overview duration. Colours are blended in `draw()`. The badge text
   colour and border width change at the half-way point (Xft colours are
   not blended).
2. The WM loop gets an overview frame deadline (about 16 ms) only while a
   highlight moves. No timer runs otherwise.
3. The selection stays pending. Commit uses the new selection at once,
   also in the middle of the highlight animation. Hide stops it.
4. Gate: `animations = 1`, `reduce_motion = 0` and a duration above 0. It
   does not need the compositor, because the WM draws it.

Tests:

- Headless: `animation_overview_ms` parsing.
- New `make check-overview-animation`: real daniwm with WM-managed dani-comp
  on a private Xvfb, copy and Present paths.
  - Fade in: an early frame blends the workspace and the overview colours,
    then the exact overview frame.
  - Escape: another client can grab the keyboard at once while the exit
    image still blends; a click during the exit fade reaches the window
    below.
  - Commit with real keys: desktop and focus change at once; the exit image
    follows the user decision.
  - Click during the fade in: measured and reported.
  - A plain override-redirect window stays instant.
  - Toggle storm: at most one overview image, XRes back to the baseline.
  - Instant cases: duration 0, reduced motion, animations off.
  - Open while another client holds the keyboard (grab failure).
  - Escape restores the same desktop and focus.
  - Bypass count with `DANI_COMP_DEBUG`; one mutation check for each
    policy row.
  - A run with `compositor = 0` for the 4B highlight only.
- 4B: the headless `test/test-overview.c` checks the blend, the half-way
  switch, the deadline and that no timer runs when off. The Xvfb test
  samples a card colour between the two ends.
- `check-overview` and all existing suites must pass unchanged.

## Results

All checks pass on a private Xvfb. `make check-overview-animation` passed 3
runs in a row after the last change (each run does the copy path, the
Present path, and a run with `compositor = 0`). Copy numbers are first, Present numbers are second. Test
config: `animation_overview_ms = 250`, `animation_open_ms = 100` (so the WM
sends hide reasons), bar on with `bar_bg = 00ff00`, overview colour at the
probe `00c600`.

| # | Check | Result |
| --- | --- | --- |
| 1 | Open: blend frames, then the exact overview frame | PASS (settle 253 / 280 ms) |
| 1 | Open: the early frame still shows the workspace below | PASS (`c60600` / `cb0100`) |
| 1 | Open: linear blend over the live workspace, no build-up | PASS (crossover 122 / 119 ms after the first blend) |
| 1 | Open: the WM holds the keyboard grab at once | PASS |
| 2 | Escape: keyboard free at once while the overview image is still on screen | PASS (free after 3 / 2 ms) |
| 2 | Escape: a real click during the fade-out reaches the client below | PASS |
| 2 | Escape: the fading image is drawn above the bar | PASS (bar pixel shows `00c600`, not `00ff00`) |
| 2 | Escape: fade-out, then the exact final frame | PASS (settle 254 / 262 ms) |
| 2 | Escape: the same desktop and focus as before | PASS |
| 3 | Commit with real keys: desktop and focus change at once | PASS (8 / 3 ms) |
| 3 | Commit: the overview fades out over the new workspace | PASS (settle 245 / 273 ms) |
| 3b | 4B: the highlight moves through between colours; selection still pending | PASS (48 of 48 changed pixels) |
| 5 | Plain override-redirect window: instant map and unmap | PASS |
| 5b | Menu mapped while the overview is open: instant | PASS |
| 5c | Open while another client holds the keyboard: no overview image stays, keyboard free | PASS (0 overview frames in 400 ms) |
| 6 | Toggle storm (10 toggles 30 ms apart): exact final frame, keyboard free | PASS |
| 7 | `animation_overview_ms = 0`: instant open, close and highlight | PASS (0 of 48 between) |
| 7 | `reduce_motion = 1` and `animations = 0`: instant open and close | PASS |
| 8 | dani-comp XRes after all of the above | PASS (38/38 and 40/40; pixmap bytes equal) |
| — | No unexpected `dani-comp: Xerror` line | PASS (both paths) |
| — | `compositor = 0`: the highlight still moves through between colours | PASS (336 of 336 changed pixels) |
| — | `compositor = 0`, duration 0: instant highlight | PASS (0 of 336 between) |
| — | Bypass count, 3 runs with `DANI_COMP_DEBUG=1`, both paths | 0 (`overview bypass` lines) |
| — | Headless: setting 0..250; role set before map and cleared on hide; highlight weights, end, no timer when off, commit mid-move, hide mid-move | PASS (`make check-animation`, `make check-overview`) |

Existing suites after the last change: `check-animation`,
`check-animation-xvfb`, `check-animation-wm`, `check-layout-animation`,
`check-layout-prototype`, `check-overview`, `check-ws-capture`,
`check-ws-render`, `check-workspace-animation`, `test/test-comp.sh`,
`test/test-comp-present.sh` and `test/run-all.sh` (268 PASS, 0 FAIL) pass.
Both binaries build with no warnings; dani-comp also builds without XPresent.

Mutation checks (copy path). Each change to dani-comp or the WM made its
own row fail. The source was restored after each one (sha256 checked).

| Mutant | Failing row |
| --- | --- |
| Overview counts as an opaque occluder while it fades | Open: linear blend, no build-up |
| Any published role marks any override window | Menu mapped while the overview is open |
| Desktop change clears the overview image | Commit: fades out over the new workspace |
| Workspace hide clears the overview image | Commit: fades out over the new workspace |
| Overview image drawn below the bar | Escape: drawn above the bar (and four more rows) |
| Highlight jumps (4B) | Highlight moves through between colours |
| WM keeps the overlay after a failed keyboard grab | Open with the keyboard held elsewhere (and the rows after it) |

## Findings

1. **The click during the fade-in is the main input/visual mismatch.** A real
   click 20 ms after the open key opened a workspace card (desktop 0 → 1).
   At that moment the probe showed `c50700` on copy and `cc0000` (pure
   workspace) on Present. The user then sees the workspace but clicks the
   overview. The 250 ms cap and the default 0 keep this opt-in and short.
   This is the risk the user accepted when choosing the setting.
2. **The fade-out is safe for input.** The overview window is destroyed at
   once, so the keyboard is free in 2–3 ms and clicks reach the window
   below. Only dani-comp draws the old image.
3. **The opaque-occluder rule needed a role exception.** Without it, the
   overview builds up on the last frame: the colours cross over 45 ms after
   the first blend, not about 120 ms (mutant run). The crossover check
   catches this.
4. **4B redraws the whole overview each frame (16 ms).** `draw()` reads the
   title property of each client card. For a 250 ms move this is about 15
   redraws. No timer runs when no highlight moves (headless check).
   The WM loop also checks the highlight deadline at the top of each event
   loop pass, so a steady stream of events does not stop the frames
   (`src/main.c`; not tested under a steady event stream).
5. **Present paces frames.** The first blend shows at about 25–34 ms on
   Present and 6–16 ms on copy. Assumption, not measured: this comes from
   Present on Xvfb, as in 3-I.

## SKIP and open items

- Hardware pacing, tearing and real input latency: SKIP (Xvfb). A Xephyr or
  real-session check by the user is still open.
- Multi-monitor overview placement is not tested here (one Xvfb screen).
- A full-monitor image at 3840×2160 uses 33,177,600 of 33,554,432 pool
  bytes. If the pool is not empty, the hide is instant (debug log
  `overview bypass: pool`). Not tested at that size.

## Decision: gate PASS (reviewed 2026-11-23)

The user reviewed the results and passed the gate. The click during the
fade-in (Findings 1) is the accepted risk of the opt-in 0..250 ms setting.
Hardware pacing, tearing, real input latency and multi-monitor placement are
still open for a real-session check.

## Real-session check (2026-11-24)

The user installed commit `591a2de` (`sudo make install`) and restarted
daniwm in the live session. The live config:
`compositor = 1`, `animations = 1`, open 140, close 100, layout 0,
workspace 180, overview 180 ms. The standalone dani-comp from
`autostart.sh` was stopped first, so the WM started its own dani-comp.

- Checked by the assistant (read-only): dani-comp ran as
  `--wm-settings` from `/usr/local/bin/dani-comp`; the root properties
  showed workspace 180 and overview 180; dani-comp was still running after
  the user's tests.
- User result for workspace switches and window fades: "Mượt rồi" (smooth).
- User result for the overview: "ok rồi" (works). The user's config replaces
  the default key binds, so `bind = mod+o:overview_toggle` was added to it
  first.
- Not measured: frame-time numbers, input latency in ms, and tearing as a
  number. The result is the user's visual judgement. Multi-monitor was not
  reported.
