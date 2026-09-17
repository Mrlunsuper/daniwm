# dani-comp Performance Audit

**Date:** 2026-09-16 13:44:05 +07
**Verified:** 2026-09-16 13:57:25 +07 — every line reference re-checked, all measurements re-run, one finding corrected (P2), one new finding added (C8).
**Scope:** `src/comp.c` (990 lines) — performance. C8 is a correctness bug found during verification.
**Method:** Full source read + Xvfb measurement (fixed damage load, several screen sizes).
**Binary under test:** `dani-comp` built 2026-09-16 13:24:45, from `src/comp.c` modified 13:24:44 (binary is current).

## Measurement

Test load: one 400x300 client fills a 20x20 box about 100–200 times per second for 5 seconds.
The damage size is the same in all runs. Only the screen size changes.
Compositor started with `DANI_COMP_DEBUG=1` to get paint counters.

| Screen | Xvfb CPU (ticks / 5 s) | Paint latency | Paints | Damage events |
|---|---|---|---|---|
| 800x600 | 27 | 0–1 ms | 860 | 860 |
| 1920x1080 | 69 | 1–2 ms | 790 | 790 |
| 2560x1440 | 164 | 2 ms | 860 | 860 |
| 3840x2160 | 21 | 3–7 ms | 30 | 1–10 (see C8) |

Two results:

1. The server cost grows with the screen area, not with the damage area (P1).
2. Paints equal damage events. The compositor paints once per damage event with no throttle (P2).

The 3840x2160 row is not a saturation result. The compositor stopped receiving damage events.
The cause is finding C8, triggered by the `XSync` that debug mode adds to every paint.

## Findings

| # | Severity | Issue | Location |
|---|----------|-------|----------|
| C8 | **High (correctness)** | `select()` blocks while events already wait in the Xlib queue; damage is never subtracted, the window freezes | `src/comp.c:849`–`:852` |
| P1 | High | No damage region: every frame repaints the full screen | `src/comp.c:524`, `:595`, `:610`, `:871` |
| P2 | High | No frame throttle for content damage (1 paint per damage event measured) | `src/comp.c:987`, `:841` |
| P3 | Medium | A window move destroys and rebuilds the window pixmap | `src/comp.c:927` |
| P4 | Medium | No occlusion culling: covered windows still composite | `src/comp.c:540` |
| P5 | Low | Shadows blend over the full window area, not the ring | `src/comp.c:384`, `:576` |
| P6 | Low | Young windows re-read the shape every frame for 500 ms | `src/comp.c:567`, `:618` |
| P7 | Low | Paints to the root window, not the Composite Overlay Window | `src/comp.c:610` |

### C8 — select() ignores the Xlib event queue

The main loop blocks in `select(xfd + 1, ...)` with no timeout when nothing fades
(`src/comp.c:849`–`:852`). It checks `XPending` only after `select` returns (`src/comp.c:859`).

Xlib reads events from the socket during every round trip. `repaint()` makes round trips:
`XGetWindowAttributes` (`:410`, `:504`), `XQueryTree` (`:479`), `XShapeGetRectangles` (`:170`),
and `XSync` in debug mode (`:626`). A `DamageNotify` that arrives during one of these round trips
lands in the Xlib queue, not in the socket. `select` then blocks. The damage region stays non-empty,
so the server sends no further `DamageNotify` for that window (ReportNonEmpty semantics).
The window content freezes until some other event arrives.

Evidence (Xvfb 3840x2160, client made ~500 fills in 3 s):

| Mode | Damage events received | Largest gap between events |
|---|---|---|
| debug (`XSync` per paint), no fix | 3 | 2680 ms (frozen) |
| debug, with fix | 351 | 20 ms |
| normal (`XFlush`), no fix | 342 | 24 ms |
| normal, with fix | 373 | 19 ms |

A minimal Damage listener (create damage on map, subtract on each event, with and without
Composite redirect) received 824–852 events in the same test. The stall is in dani-comp.

In normal mode this test did not trigger the freeze. The round trips at `:410`, `:479`, `:504`
and `:170` open the same window, so the risk exists in normal mode too. I did not observe it there.

Fix (verified in a patched copy): before `select`, check the queue.

```c
if (XPending(dpy)) { tv.tv_sec = 0; tv.tv_usec = 0; tvp = &tv; }
```

Then treat `ret == 0` with a zero timeout as "process the queue". Or simpler: drain `XPending`
first, and call `select` only when the queue is empty.

### P1 — No damage region

One character typed in a terminal costs 3 full-screen operations:

1. a full-screen copy of the wallpaper into the back buffer (`src/comp.c:524`),
2. a full composite of every mapped window (`src/comp.c:595`),
3. a full-screen blit to the root (`src/comp.c:610`).

`XDamageSubtract(dpy, dw->damage, None, None)` (`src/comp.c:871`) discards the damaged area.
The 4th argument `parts` receives the damaged region (`/usr/include/X11/extensions/Xdamage.h:65`).

Fix:

1. Pass an `XserverRegion` as the last argument of `XDamageSubtract`.
2. Union the regions of all events in the drain loop.
3. Set that region as the clip on `back_pict`.
4. Copy only its bounding box in the background step and the present step.

XFixes is already included and linked (`src/comp.c:34`, `Makefile:17`). No XFixes call exists yet,
so the region API adds no new dependency. This is the largest win.

### P2 — No frame throttle for content damage

Corrected during verification. The first version of this audit pointed at the `!dirty` test in
`src/comp.c:841`. That test is dead code: `repaint()` resets `dirty` (`:634`), and every
handler that sets `dirty` runs inside the drain loop that ends with `if (dirty) repaint();`
(`:987`). So `dirty` is always 0 at the top of the loop.

The unthrottled path is `:987`. It repaints after every drain with no check of `last_paint`.
Measured result: paints equal damage events at every screen size (table above).
Video, a game, or a fast terminal drives full-screen repaints as fast as the server accepts them.

Fix (two edits):

1. At `:987`: repaint only if `now_ms() - last_paint >= 16`. Otherwise leave `dirty` set.
2. At `:841`: drop `&& !dirty`, so the top of the loop sleeps the remaining time and then paints.

### P3 — Move destroys the window pixmap

`ConfigureNotify` always calls `win_free_pix(w)` (`src/comp.c:927`). The next frame then runs
`XGetWindowAttributes`, `XCompositeNameWindowPixmap`, `XRenderCreatePicture` and
`XShapeGetRectangles` again (`src/comp.c:407`–`:425`).

The Composite specification says storage is reallocated only on a size change:
"Storage is automatically reallocated when the top level window changes size"
(`man 3 XCompositeNameWindowPixmap`, section Per-hierarchy storage).

`XConfigureEvent` carries `x, y, width, height, border_width` (`/usr/include/X11/Xlib.h:764`–`:776`).

Fix:

1. Compare `e->width`, `e->height` and `e->border_width` against the cached values.
2. Free the pixmap only if one of them changed.
3. For a pure move, update `w->x` and `w->y` from the event and keep the pixmap.

A drag then costs no pixmap work per frame.

### P4 — No occlusion culling

A fullscreen opaque window still makes every window below it composite, and the wallpaper copy
still runs (`src/comp.c:540` loop).

Fix:

1. Walk the stack from top to bottom first.
2. Track the region covered by opaque windows (alpha >= 0.999, not shaped).
3. Skip any window that the covered region hides completely.
4. Skip the background copy where the coverage is complete.

### P5 — Shadow overdraw

Each of the 3 shadow layers composites a rectangle larger than the window (`src/comp.c:384`).
The window then overdraws the centre, so most of that blending is wasted.

Fix: clip each layer to the 4 ring rectangles around the window.

### P6 — Shape polling on young windows

For 500 ms after a map, each frame calls `XShapeGetRectangles` for that window (`src/comp.c:567`)
and forces `fading = 1` (`src/comp.c:618`). Measured: one mapped window with 1 damage event
produced 30 paints (`paint#31` in the 4K logs) — the 500 ms burst at 60 Hz.

Fix: do the recheck once or twice (for example at 100 ms and 400 ms), not every frame.

### P7 — Root window as the target

The code already requires Composite >= 0.3 (`src/comp.c:752`), so `XCompositeGetOverlayWindow`
is available. The overlay window removes the wallpaper snapshot code, the `bg_dirty` root-clear
fallback, and the Expose handling (`src/comp.c:953`).

This is a larger change. Do it after P1–P3, and only if you want the wallpaper code gone.

### Minor items

- `win_get` is a linear list scan, called once per window per frame (`src/comp.c:144`).
- The shaped clip array is allocated and freed for every window on every frame (`src/comp.c:581`).

Both are small next to the round trips above. Fix them after P1.

## Suggested order

1. C8 — a few lines, fixes a freeze.
2. P2 — two small edits, caps the worst case.
3. P3 — small change, removes the drag cost.
4. P1 — the real fix, largest effort.
5. P4, P5, P6.
6. P7 if you want the wallpaper code removed.

## Notes

- This audit did not re-check the Round 5 correctness fixes in `AUDIT.md`.
- Test tools are not in the repository: a 45-line X client that fills a 20x20 box in a loop,
  a 35-line Damage listener, and a copy of `comp.c` with env-gated debug switches. They live
  in the session scratchpad. Ask if you want them under `test/`.

---

## Implementation status — 2026-09-16 14:44:02 +07

All findings except P7 are implemented in `src/comp.c`. Build is clean
(0 warnings under the project flags). All 13 headless suites pass
(`test/run-all.sh` -> RESULT: ALL PASS); `test/test-comp.sh` 7/7.

| # | Status | Notes |
|---|--------|-------|
| C8 | **Done** | Drain the Xlib queue before `select`; block only when the queue is empty. |
| P1 | **Done** | Track a damage bounding box (window-local damage translated to root, inflated by border+2). Content-only frames copy the wallpaper, composite, and present only that box. Full frame on stack/geometry/appearance change. |
| P2 | **Done** | Repaint at most once per 16 ms; the throttle drives the `select` timeout. Measured: ~281 paints/5 s vs ~891 before. |
| P3 | **Done** | `ConfigureNotify` frees the pixmap only when size or border changed; a pure move keeps the pixmap and updates x/y. |
| P4 | **Done** | Find the topmost opaque window that fully covers the paint box; skip every window below it and skip the wallpaper copy. |
| P5 | **Done** | Clip each shadow layer to the 4 ring rectangles (skip the covered center). |
| P6 | **Done** | Recheck a new window's shape only at 100 ms and 400 ms, scheduled through the `select` timeout — no more forced 60 Hz for 500 ms. |
| P7 | **Skipped** | Assessed and deferred. Reasons below. |
| minor: clip buffer | **Done** | Reuse one grown buffer for the shaped clip rects instead of malloc/free per window per frame. |
| minor: win_get hash | **Skipped** | The tracked-window list has at most a few dozen entries and each frame is now clipped/occluded, so the O(n) scan is negligible. A hash adds state and lookup-bug risk for no measurable gain. |

### Measured effect (same 20x20 damage load, 5 s)

| | Before | After |
|---|---|---|
| Paints | ~891 | ~281 |
| Xvfb CPU 1080p / 4K | ~25 / ~25–151 | ~4 / ~5 |
| Server cost vs screen area | scales with area | flat |

### Correctness checks (Xvfb, pixel probes)

- Overlapping windows + partial damage: occlusion order, overlap, and the
  damaged sub-rect all render correctly.
- Fullscreen opaque occluder: the hidden window stays hidden, the damaged
  spot renders, the wallpaper copy is skipped.
- Shaped window whose shape is set 250 ms after map: detected (P6 recheck).
- 4K no longer freezes, in normal and debug mode (C8).

### Why P7 was skipped

1. The audit over-stated it: the overlay window covers the whole screen above
   the real root, so the wallpaper must still be drawn onto it. The
   wallpaper-snapshot code does not go away.
2. The overlay is usually 32-bit; `back_buf` is the root's 24-bit. The present
   uses `XCopyArea`, which needs matching depths, so it would fail on the
   overlay. P7 needs a rewrite of the working present path to
   `XRenderComposite`, plus input pass-through and overlay release on exit.
3. It gives no measurable performance benefit — every performance win is
   already in P1/P2/P4/C8. The change is architectural only, on a
   currently-working, fully-tested path.

Decision (owner): skip P7.
