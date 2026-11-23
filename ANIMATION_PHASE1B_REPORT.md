# Phase 1B — fade-out with explicit lifecycle reasons

Status: **implemented; private automated exit gate PASS.** No installation,
active-session restart, live-display probe, or live configuration change.
Phases 2–4 are not implemented or approved by this work.

## Behavior and policy

- Adds only `animation_close_ms` (default 100, integer 0..1000). Opening duration
  remains independent. WM effects require compositor=1, animations=1 and no
  reduced motion. Zero disables the corresponding effect.
- WM startup remains opt-in/instant by default. Reload snaps live fades and
  removes closing layers without restarting the compositor. Missing/invalid
  closing settings fail instant; the existing five-word opening contract stays
  compatible with Phase 1A readers.
- Standalone opening defaults remain unchanged. Closing defaults OFF; the new
  `--close-ms N` explicitly opts in on an unmanaged display. If a supporting WM
  is detected, standalone closing is conservatively skipped: use compatible
  daniwm with `--wm-settings` for lifecycle-aware closes.
- Application close/withdraw fades a detached, already-painted image when safe.
  WM workspace, monocle, external desktop move, scratchpad parking and reload
  hides remain instant. Their remaps do not restart opening fades when tracked
  hide state is available. Generic override-redirect windows (including overview
  teardown), docks and fullscreen are excluded.
- Client destruction/removal, mapping, focus, tiling, desktop/EWMH state and
  input delivery remain immediate. No real client geometry animation, per-frame
  resize, input window/shield, grab, thread, or WM animation timer is added.

## Image ownership decision

The Phase 0 copied-ARGB experiment remains test-only. Phase 1B instead transfers
an existing named window pixmap/Picture reference from `Win` into `ExitPool`.
XComposite named-pixmap storage survives unmap/destruction while those references
remain owned by the compositor. Private tests explicitly destroy the application
BEFORE detaching the image and verify its border/content remains readable.
This avoids an extra live image cache, duplicate full-size allocations and
periodic capture timers. There is no WM capture request/acknowledgment.

Only an already-painted candidate with validated server geometry is eligible.
At detachment the compositor drains its own pending Render errors with XSync;
this is not a window query and is never done on the WM connection. The WM has
already committed removal independently and never waits for the compositor.
Failed/unpainted/invalidated content, unknown stacking, complex shapes above
256 rectangles, or exhausted budget simply skip the effect. A resize invalidates
old live content as before. Destroy-before-first-paint therefore has no ghost.

`src/exit-layer.c/.h` owns detached resources, with no `Win *`, `Client *` or
post-removal application queries. Keys are `(identity, generation)`; tracked
lifetimes/map generations are monotonic, overflow fails instant, and remap or
new CreateNotify cancels prior visuals for that identity. Stacking anchors also
include a generation. A lost/reused anchor cancels rather than elevates an old
image. Layers retain numeric rectangles, fixed alpha/dim, bounded shape data,
resource handles and animation state only.

Limits (including admission BEFORE transfer):

| Limit | Policy |
| --- | --- |
| Images | 8, reject ninth rather than queue |
| Pixel payload | 33,554,432 bytes; actual depth's server bpp, padded stride and border footprint |
| Retained lifetime | At most 1000 ms, also bounded by closing duration |
| Shape metadata | Fixed 256 rectangles/image, otherwise skip |
| Overflow/invalid size | Skip, keep normal instant cleanup |

Pixel payload does not claim to measure total GPU/driver allocation or metadata.
No images are written to disk. Pictures are released before pixmaps. Expiry runs
before paints, at loop scheduling, and while draining event storms; independent
select deadlines prevent a stalled Present submission from extending retention.
Completion, remap/reparent/new identity, workspace change/hide reasons, changed
settings, owner loss, root resize and compositor shutdown/replacement clean up.
Retained-resource X errors mark cancellation in the callback and release safely
outside it. Old bookkeeping is unpublished before issuing release requests.

Rendering preserves premultiplied ARGB and shape clipping, multiplies application
opacity and exit alpha, and uses the existing separate dim overlay. Source
transform/filter/clip are explicitly restored to identity/nearest/no clip before
reuse, and destination shape clipping is reset afterward. Layers use full-frame
repaint and disable opaque occlusion shortcuts while present. Stable stack rank
plus a live-generation anchor positions layers below surviving upper windows;
new docks/override/fullscreen content stays above them. Closing shadows disappear
instantly; this phase fades application content, not a second retained shadow.

## Narrow lifecycle notification

`src/animation-lifecycle.h` defines two bounded Phase 1B additions:

- `_DANIWM_ANIMATION_CLOSE_V1`: CARDINAL/32 root property, exactly
  `[version=1, supporting WM window, closing duration]`.
- `_DANIWM_ANIMATION_HIDE_V1`: root ClientMessage/32, exactly five words:
  `[version=1, supporting WM window, client, sequence, hide reason]`.
  Reasons are workspace, monocle, scratchpad and reload only.

The WM caches the atom during settings publication. Its wrapper adds only an
ordered XSendEvent immediately before the original XUnmapWindow, without a
round trip, flush, acknowledgment, delay or server grab. Disabled policy does
not send notifications. Server processing order on that SAME connection ensures
reason precedes the resulting unmap event; an independent test observer checks
this ordering. This is not a claim about cross-connection capture ordering.

The compositor validates format/version, live supporting owner, payload bounds,
allowed reasons, and serial-number ordering (including wrap and stale rejection).
The reason is attached only to the currently tracked generation, cleared on
remap/owner replacement, and outgoing layers are cancelled on valid hides.
Duplicate StructureNotify/SubstructureNotify delivery is filtered to root events
for map/unmap, preventing a duplicate map from restarting a suppressed fade.
Synthetic unmaps cannot create an exit image. All of this is a trusted X-client
contract, not an authentication/security boundary. No Phase 2 capture/geometry
protocol, prepare/ready handshake or transaction queue is added.

## Verification

| Check | Result |
| --- | --- |
| `make -B -j2 all` | PASS, no normal compiler warnings |
| `make check-animation` | PASS production pool: count/byte/TTL, 10000 cycles, cancellation, overflow/stride, generation sampling and payload/settings validation |
| `make check-animation-xvfb` | PASS copied prototype plus production exit pixel/resource/state tests; copy and actual Xvfb Present real-loop paths |
| `make check-animation-wm` | PASS no-comp/manual-comp baselines and WM-managed settings/fades/reload/focus; ordered workspace/monocle hides instant |
| `make check-swipe check-overview` | PASS headless and private Xvfb |
| `bash test/test-comp.sh` | PASS including close CLI invalid/overflow rejection |
| `bash test/test-comp-present.sh` | PASS all seven buffer/serial/timeout/fallback assertions |
| Full `test/run-all.sh` | PASS 42/42 suites in UID-preserving private Bubblewrap/Xvfb sandbox |
| ASan + UBSan animation targets | PASS headless, Xvfb production renderer/loop and WM driver, no suppressions |
| `-fanalyzer -Werror` production pool and headless close tests | PASS |
| Shell syntax / `git diff --check` | PASS; shellcheck unavailable |

Production XRes measurement at 640x480x24: after EACH of 256 lifecycle cycles,
**16 resources / 2,457,600 pixmap bytes**, exactly equal to warmed baseline.
Separate tests reach 8 retained images and the full 32 MiB payload, inject a
retained-Picture error, preserve border/shape/ARGB, verify opacity/dim/stacking,
and demonstrate no-image/reduced/zero/disabled fallback. XID reuse is covered
through deterministic generation/cancellation tests and actual unmap/remap;
forcing the X server allocator to recycle a numeric XID was not attempted.

Nine real-loop modes run on copy and Xvfb Present (18 runs): fade, instant,
zero, reduced, opening cancellation, close, close cancellation, rapid remap and
32-window close bursts (3 ms spacing). Each finishes with **zero settled
paints/events/timer wakeups over 1500 ms**. Closing/cancel/remap peak at one
detached image; bursts peak at 4 copy / 8 Present images in the recorded run,
never exceed admission limits, and all modes return to zero bytes.
The normal 5-second GC maintenance timer remains, outside that observation
window. A 1000 ms close produced roughly 123 copy submissions / 63 Present
submissions including startup in one run, consistent with the existing 8 ms
limiter and Xvfb software Present cadence, NOT a hardware-vsync guarantee.

Logs in this session:

- `/var/tmp/daniwm-phase1b-all.log` — 42-suite aggregate, `RESULT: ALL PASS`.
- `/var/tmp/daniwm-phase1b-build-final.log` — full normal rebuild.
- `/var/tmp/daniwm-phase1b-final-render.log` — final normal renderer/loop tests.
- `/var/tmp/daniwm-phase1b-check-final.log` — math, WM, swipe, overview.
- `/var/tmp/daniwm-phase1b-sanitizers-final.log` — sanitizer checks including WM driver.
- `/var/tmp/daniwm-phase1b-sanitizers-render-final.log` — final pool/renderer/18-loop sanitizer checks, including bursts.
- `/var/tmp/daniwm-phase1b-comp.log`, `/var/tmp/daniwm-phase1b-present.log`.

A normal link initially encountered a mixed sanitizer object after a test's
recursive make rebuilt a changed file; a full normal rebuild resolved it.
The sanitizer run itself passed. Historical live acquisition BadMatch races
remain possible in the old manual-compositor semantic baseline; they are logged
by existing traps, skipped safely, and are not claimed fixed by exit retention.

## Exit gate and unavailable validation

The automated Phase 1B exit gate is satisfied: resource-stable repeated closes,
no stale remap generations, explicit instant workspace/monocle hiding, and
unchanged focus/removal/geometry/input regression behavior. Missing images skip
instead of delaying logical state. Documentation/config add only this phase's
setting; user work from prior phases remains intact.

Hardware tearing, subjective fade quality, multi-GPU/driver pixmap lifetime,
and real physical input latency are **SKIP/not verified** by Xvfb. No live or
hardware-backed display was tested. Animations remain opt-in; separate approval
is required for any installation, session restart or such validation. Phase 2
must still prototype/review its capture coordination and input tradeoff before
full integration.
