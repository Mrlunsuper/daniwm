# Phase 2 — best-effort layout movement report

## Decision: best-effort release gate PASS on a private Xvfb; strict gate NOT met

The opt-in best-effort path (`animation_layout_ms > 0`) passes the best-effort
release gate in `ANIMATION_PLAN.md` §4 Phase 2. The original strict gate stays
unmet: X can send a click or hover to the final geometry while the picture
still moves. This report does not claim that the strict gate passed.

The default stays instant (`animation_layout_ms = 0`). Nothing was installed.
The live session was not restarted. All tests used new private Xvfb displays.

## Bug found and fixed

The new test found one bug in `src/comp.c`.

- The WM sends `XMoveResizeWindow` before it sets `_DANIWM_LAYOUT_ANIMATION_V1`,
  on the same connection. Thus dani-comp always gets the ConfigureNotify
  before the PropertyNotify of the same arrange.
- The old ConfigureNotify branch looked up old positions in the *previous*
  transaction. Nothing cleared that data after use.
- Result 1: a second swap of the same windows showed no movement. The picture
  started at the stale old position, which was equal to the new target.
- Result 2: a mouse drag of a window from the last transaction animated from
  a stale tile position. The plan requires direct mouse manipulation to stay
  instant.

Fix:

- Remove the stale lookup from the ConfigureNotify branch.
- In `layout_transaction_read()`, start movement only for windows that already
  moved. Clear `layout_pending` after each entry.
- Remove the unused per-window arrays from `LayoutRequest`.

Known effect of the fix: if dani-comp reads a newer transaction before it
gets the matching ConfigureNotify, that movement is instant. This is the
fail-closed path. It is not a visual error.

## Test: `make check-layout-animation`

Files: `test/test-layout-animation.sh`, `test/layout-animation-xvfb.c`.

Setup: WM-managed dani-comp, Present off, 1024×768, two tiled clients in one
column, `border = 0`, `gaps_on = 0`, `bar_on = 0`, `focus = click`. The
duration is 1000 ms, so that a sample at 60 ms is stable.

| Check | Result |
| --- | --- |
| A swap commits the final X geometry immediately | PASS |
| First swap: old picture still in the top slot at 60 ms | PASS |
| First swap: a gap shows at row 10 mid-transit (pictures move) | PASS |
| First swap: final picture matches final geometry after settle | PASS |
| ConfigureNotify per client for one 1000 ms swap (≤ 2) | PASS (1 and 1) |
| Second swap of the same windows moves again (60 ms and gap checks) | PASS (FAIL before fix) |
| Mouse drag: picture follows real geometry within 60 ms | PASS (FAIL before fix) |
| Mouse drop swaps the tiles; pictures match within 60 ms | PASS |
| 20 rapid swaps each change the real geometry | PASS |
| dani-comp XRes count and pixmap bytes return to baseline | PASS (38/38, 8388608/8388608) |
| `animation_layout_ms = 0` keeps swaps instant | PASS |

The gap check polls pixel (512, 10) for 500 ms. Mid-transit, neither tile
covers that row. An instant path or a frozen picture never shows a gap there.

The test passed 7 times in a row after the fix (3 runs with the gap check).

Mouse input is synthetic: `ButtonPress`/`ButtonRelease` go to root with
`SubstructureRedirectMask`, and `XWarpPointer` makes real motion. This
machine has no libXtst. This is not proof of physical input.

## Regression results

- `make check-animation`: PASS.
- `make check-animation-xvfb`: PASS.
- `make check-animation-wm`: PASS.
- `make check-overview`: PASS.
- `test/test-comp.sh`, `test/test-comp-present.sh`: PASS.
- `test/run-all.sh`: ALL PASS (268 PASS lines, 0 FAIL).
- `make check` stops at its first step, `check-ai`, so `run-all.sh` was run
  directly. `check-ai` fails 5 of 35 checks. It tests the Python `dani-ai`
  tool, which this branch does not change. One visible cause is the removed
  `SourceFileLoader.load_module` in the installed Python.

## Gate mapping (`ANIMATION_PLAN.md` Phase 2 best-effort release gate)

| Gate item | Status |
| --- | --- |
| Final geometry matches the instant path | PASS |
| No per-frame client resize storm | PASS |
| Click/hover mismatch is documented and accepted | DOCUMENTED (README, plan) |
| Retained resources settle with no stale layers | PASS |
| Strict: clicks/hover never target stale positions | NOT MET (by design) |

## SKIP / not verified

- Click and hover during movement: SKIP. The mismatch is a known limitation.
- Hardware pacing, tearing and frame-time distribution: SKIP. Xvfb cannot
  show them.
- Known gap against the plan bullet "cancel for direct mouse manipulation":
  the fix stops *new* stale animations, but it does not cancel a movement that
  is already in flight. If a drag starts while the picture still moves, the
  picture finishes its current movement (at most `animation_layout_ms`) before
  it follows the pointer. This does not block the best-effort release gate.
- Resize, ARGB, shaped and translucent clients: instant by design. Not part
  of this test.
