# Phase 2 follow-up — conservative eligibility review

## Decision: no safe movement eligibility established; remain BLOCKED

Requested scope: resolve the Phase 2 prototype blocker, rather than advance to
Phase 3. This follow-up evaluates conservative admission conditions and records
a fail-closed test policy. **It does not resolve the blocker for useful layout
movement, does not complete Phase 2, and does not authorize full integration.**

A sampled condition is not a guarantee over the lifetime of an image. The tested
conditions cannot establish that input sees the final picture before delivery.
The safe behavior presently justified is **instant geometry and presentation**
for every geometry-changing transaction. A no-op is instant too; it is not a
successful animation and must not be counted as passing the Phase 2 exit gate.

No production sources, configuration keys, launcher flags, README, roadmap, or
live session files were changed by this follow-up. Previous work is preserved.
No server/input grabs, replay, new windows acting as input shields, delayed WM
state commits or per-frame client resize requests were added.

## Changes

Created:

- `test/layout-eligibility.h`: explicit **test-only** fail-closed decision model.
  Invalid payloads are rejected; disabled/zero/no-op states are instant; every
  valid move/resize is classified as unproven input geometry. There is
  intentionally no eligible-animation result in this model.
- `test/test-layout-eligibility.c`: 48 move/resize observation combinations,
  disabled/reduced/zero paths, no-op distinctions and invalid-input cases.
- This report.

Extended:

- `test/layout-prototype-xvfb.c`: pointer entry, transparent-picture, visible
  cover withdrawal, revocable empty input shape and instant-control experiments.
- `test/test-layout-prototype.sh`: runs the new headless checks for both
  `make check-animation` and `make check-layout-prototype` via existing targets.

The previously created `Makefile` targets are unchanged in this follow-up.
The decision model and experiments are not linked into daniwm or dani-comp.
The original `ANIMATION_PHASE2_PROTOTYPE_REPORT.md` remains historical evidence;
this report adds the follow-up findings, not a retroactive PASS.

## Required safety property

For a visible animated application, server-delivered pointer coordinates and
window-local interaction must agree with the displayed rectangle/content before
that interaction is delivered. Preserving only the top-level XID is insufficient:
scaling/moving content changes the apparent location of controls within a client.

Ordinary X11 delivery is server-side; the compositor asynchronously observes raw
input and issues rendering requests on another connection. The server need not
wait for that rendering before sending the application its input event. A final
pointer query, drain of already-queued events, larger exclusion margin, short
animation duration or more frequent polling does not create such a barrier.

Changing real geometry every frame would instead violate single-commit/no-resize-
storm constraints. Delaying/replaying input, maintaining shields/grabs, delaying
logical geometry, or altering focus rules would violate semantic constraints.
None was implemented as a workaround.

## Candidate review and prototype findings

Experiments use only a newly allocated private Xvfb 640x480x24 display. The driver
uses independent WM/compositor/application/input X connections in one process to
impose adverse but legal processing schedules. XTEST generates actual routed
button events; XSendEvent is not used to fake application button delivery.
These are counterexample schedules, **not estimates of race frequency**.

### 1. Pointer outside the affected swept rectangle at admission — REJECT

The test places the pointer at (600,420), outside both clients' old, target and
swept rectangles, and verifies it with XQueryPointer. After the real geometry
commit and the old-image paint, the pointer enters (80,100) and clicks before the
compositor processes its raw event notifications.

**8/8 clicks reached final-position green B while red A remained displayed.**
Raw motion and raw button notifications were present afterward, but did not gate
application delivery. This addresses the previous suggestion to skip only when
the pointer is initially near the changed region: that condition is insufficient.

### 2. Input-transparent visual layer — REJECT as a solution

The fixture already renders retained pictures directly onto the root; there is
**no animated input window at all**. This is input-transparent without a new
shield or any change to client delivery. The same 8/8 counterexamples remain.

Transparency prevents intercepting input; it does not align the picture with the
real client. No extra overlay window or modified production input shape is needed
or justified by this candidate.

### 3. Animation hidden by an ordinary visible covering application — NOT CERTIFIED

An ordinary blue visible application covers the entire affected client area.
It is a positive control for normal stacking/input: while mapped, its own window
receives the button and is visibly blue. It is not a transparent or invisible
animation shield, and its lifetime is controlled by its application connection.

The owner then unmaps that cover. Input immediately reaches the final-position
client in **8/8 schedules**, before the compositor processes the unmap event.
The displayed pixel still contains the prior blue cover until repaint.

**Interpretation matters:** this test demonstrates revocation of the sampled
cover/non-interaction condition. The blue stale frame can occur in an ordinary
compositor too; it is NOT independent proof of a transformed-image mismatch or
proof that every occlusion-only design is impossible. It does show that observed
occlusion alone is not a lifetime certificate for continuing the proposed effect.
Keeping the cover mapped until an acknowledgment, or adding an animation-owned
cover, would change lifetime/input behavior and was not attempted.

Completely occluded interpolation is also not a useful visible layout effect.
A different strictly clipped/occlusion-only design would need its own validated
invariant; no such design is approved by this experiment.

### 4. Application currently has an empty ShapeInput — REJECT sampled admission

The fixture's application owner temporarily sets its clients' input shapes empty
and the compositor verifies zero ShapeInput rectangles. After capture/commit/
old-image paint, the owner restores their default input shapes. The next routed
click reaches the final-position B while A is still displayed: **1/1 reproduced**.

Only the fixture application changes its own input shape, as an adverse stimulus.
The WM/compositor never uses input-shape changes to solve delivery. No keyboard/
focus guarantee follows from empty pointer ShapeInput either. A promised
immutable non-interactive surface would be a new, separately specified contract,
not a fact inferred from an arbitrary application at one moment.

### 5. Instant final-rectangle presentation — ACCEPT fallback only

After painting the exact final rectangles, the same injected ordinary press
matches the displayed B in **8/8 controls**. Input is delivered unchanged.
ConfigureNotify counts remain exactly one per affected client for each tested
transaction, including all visual test paints. No snapshots remain afterward.

This validates the safe fallback in these controlled schedules. It is not a
universal claim that X11 compositor pixels update atomically with server input,
and does not excuse the ordinary presentation latency of a compositor. The
policy avoids introducing an additional intentional geometry/content mismatch.
It does not deliver the roadmap's desired movement animation or pass its gate.

## Why no alternative production feature is introduced

- Pure moves still mismatch positions; pure resizes can mismatch local controls.
- Keyboard-triggered commands can be followed immediately by pointer input; they
  do not establish a non-interactive interval.
- Sampling no buttons pressed does not prevent the next button press.
- Shortened duration makes a race smaller, not impossible; the exit gate has no
  approved probability/latency tolerance that would allow claiming this safe.
- Pointer-outside, current occlusion and empty input shapes are mutable facts,
  not guarantees. Owner/generation checks protect resource identity but do not
  order display completion before future input delivery.
- Translating only decoration would be a different visual effect and not
  approval of client rectangle interpolation. No substitute effect is shipped.

A useful geometry-changing effect needs either a new demonstrable invariant
within the current boundaries or explicit requirements revision. This follow-up
does not silently choose such a revision. Existing Phase 1 fades remain separate
from moving client rectangles.

## Resource and correctness evidence

Normal and ASan/UBSan runs reproduced the same outcomes:

| Metric | Recorded result |
| --- | --- |
| Pointer-outside then entry/click | 8/8 mismatches |
| Already input-transparent root pictures | Same 8/8 mismatches (not independent trials) |
| Visible-cover admission revoked before observer processing | 8/8 |
| Empty input shape restored before click | 1/1 mismatch |
| Instant final-picture controls | 8/8 match |
| Final retained image count / charged bytes | 0 / 0 |
| XRes baseline/final | 12 / 12 resources; 2,457,600 / 2,457,600 pixmap bytes |
| Two-client capture p50 / p95 / max (52 successful captures) | 1089 / 1277 / 1402 microseconds in recorded normal run |
| Original controlled early/late capture schedules | 32 early captures / 32 late skips |
| Original transaction geometry notifications | Exactly one/client/transaction |

An initial aggregate resource assertion found **one resource, zero pixmap bytes**
added after the original baseline. The observer's XISelectEvents setup allocates
fixed server-side selection state. Moving that unchanged setup before baseline
measurement yields exact equality after every original transaction and after all
new eligibility cases. This is not a suppressed leak: the assertion remains
exact, includes observer overhead, and snapshots are independently empty.

Retained copies still use the existing test's 8-image/32 MiB admission and
1000 ms ceiling, identity-generation checks, staged X errors, full-frame
rendering and transform/filter/clip restoration. All new sequences finish well
within the retention ceiling and release their copies explicitly. Runtime visual
sampling remains monotonic-time based; fixed-time samples are deliberately frozen
counterexample frames, not a proposed frame-count animation loop.

## Verification

| Check | Result |
| --- | --- |
| `make -B -j2 all` | PASS, no warnings |
| `make check-layout-prototype` | PASS diagnostics; movement integration still BLOCKED |
| ASan + UBSan prototype | PASS, no reports/suppressions |
| `-fanalyzer -Werror` headless eligibility tests | PASS |
| `make check-animation` | PASS including 48 fail-closed observation combinations |
| `make check-animation-xvfb check-animation-wm` | PASS existing fade/snapshot/loop/WM suites |
| `make check-swipe check-overview` | PASS offline and private Xvfb |
| `test/test-comp.sh`, `test/test-comp-present.sh` | PASS |
| Full `test/run-all.sh` | **42/42 PASS** in private UID-preserving Bubblewrap/Xvfb sandbox |
| Shell syntax / `git diff --check` | PASS; shellcheck unavailable |

Logs:

- `/var/tmp/daniwm-phase2-eligibility-normal.log`
- `/var/tmp/daniwm-phase2-eligibility-sanitizers.log`
- `/var/tmp/daniwm-phase2-eligibility-build.log`
- `/var/tmp/daniwm-phase2-eligibility-regressions.log`
- `/var/tmp/daniwm-phase2-eligibility-animation.log`
- `/var/tmp/daniwm-phase2-eligibility-comp.log`
- `/var/tmp/daniwm-phase2-eligibility-present.log`
- `/var/tmp/daniwm-phase2-eligibility-full-regressions.log` — `RESULT: ALL PASS`

Hardware timing, GPU scanout ordering, subjective movement/text quality, physical
input/gestures, mixed monitors and a live WM integration remain **not validated**.
No hardware-backed or active display was probed; this is a design/prototype gate.

## Disposition

Keep production layout/discrete movement instant. Do not add `animation_layout_ms`
or claim Phase 2 completed, and do not automatically advance to Phase 3. The
fail-closed model makes the current boundary explicit and testable but certifies
no useful movement effects. Further work needs a genuinely different invariant
or an explicit roadmap/acceptance decision; repeating sampled-admission variants
alone is not a path to passing the existing strict gate.
