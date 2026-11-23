# Phase 2 — mandatory coordination and input prototype

## Gate decision: BLOCKED — stop before production integration

The mandatory prototype is implemented and its diagnostic assertions pass.
**The Phase 2 architecture/input exit gate does NOT pass.** A final-position
client receives an ordinary click while a different client's old-position image
is still visible. An XI2 raw notification is an observation, not a barrier before
application delivery. Geometry changes also generate a normal EnterNotify under
a stationary pointer without raw motion.

No production layout animation, layout setting, launcher change, capture protocol
or layout refactor has been integrated. Phase 1A/1B behavior is unchanged. This
report is the requested pause for prototype findings, not approval to proceed to
Phase 3 or a claim that Phase 2 is complete.

## Scope and files

Created:

- `test/layout-prototype.h`: test-only bounded transaction codec, identities,
  owner/epoch/sequence checks, lifecycle model and cubic rectangle sampling.
- `test/test-layout-protocol.c`: deterministic headless protocol/state/math tests.
- `test/layout-prototype-xvfb.c`: separate WM/compositor/app/input X connections,
  retained-copy rendering, adverse ordering and click/hover experiments.
- `test/test-layout-prototype.sh`: warning-as-error build and freshly allocated
  private `Xvfb -displayfd` server; never uses an inherited or guessed display.
- This report.

Modified only `Makefile`: adds `make check-layout-prototype` and runs the headless
protocol checks under `make check-animation`. The prototype includes production
`src/comp.c` under a renamed main to reuse its Render targets/error traps, but
neither production binary links or calls any prototype code. Existing dirty and
untracked work from earlier phases was preserved.

`README.md`, example `config`, `src/*`, launcher flags and live session files
were not changed in this phase. No binaries were installed and no active WM was
restarted. No application images were saved to disk.

## Inspection findings

- `src/layout.c` commits real geometry immediately via XMoveResizeWindow. Arrange
  coalescing is already implemented; tile sizing includes master/stack weights,
  borders, gaps and per-monitor geometry. This phase does not change these rules.
- `src/client.c` focus and workspace paths remain the logical authority. Tiling
  focus does not restack; floating focus may raise. Workspace transitions unmap,
  arrange and focus immediately, independently of compositor effects.
- `src/main.c` observes ordinary clicks via XI_RawButtonPress, without grabs or
  replay. Ordinary application delivery is intentionally untouched. Hover focus
  uses normal EnterNotify plus an existing stationary-pointer lock for specific
  lifecycle actions, not a layout animation barrier.
- `src/mouse.c` owns direct manipulation and existing pointer grabs. Reusing those
  grabs to gate ordinary animated-window input would change semantics and was
  not attempted. No new pointer/keyboard/server grab or input shield was added.
- `src/overview.c` owns a bounded thumbnail cache and overlay interaction. It is
  not a workspace/layout capture buffer and is not used by this experiment.
- Existing compositor traps synchronize only their own connection. Request order
  on the WM connection does not force another process to capture before resize.

## Experimental protocol and state model — NOT frozen API

All new atoms are explicitly test-only (`_DANIWM_TEST_LAYOUT_*`). The experiment
uses a selection-owner capability marker and owner epoch. Payloads are exact-size
CARDINAL/32 properties, with a ClientMessage envelope identifying operation,
sequence, epoch and property holder. The receiver checks that a queued envelope
still matches the property; overwritten older messages are rejected rather than
silently rebound to newer contents.

One batch is bounded to **8 clients**, **8 header + 11 words/client** (at most
96 words). Each record carries a window identity, 64-bit generation, old rectangle
and target rectangle. There is no unbounded queue. Validation rejects unknown
owners/epochs/version/operation, extra or truncated data, non-wire LP64 bits,
duplicate identities, zero generations, invalid rectangles and duration overflow.
Serial comparison rejects stale messages, including across wrap. Same-sequence
READY/COMMIT must match every batch field and generation.

The model has PREPARE, READY, COMMIT and cancellation, a **16 ms experimental
prepare deadline**, a **1000 ms retention ceiling**, newest-sequence replacement,
zero-duration bypass, backwards-clock cancellation and exact completion.
Late READY after bypass cannot revive a transaction. The 16 ms value is a test
parameter, NOT a proposed WM delay or an approved production timing guarantee.

Retained copies use an independent compositor-owned ARGB32 pixmap/Picture pair,
admitted against existing **8 images / 32 MiB** limits before allocation. Actual
server ARGB bpp/scanline format is checked. Creation and copies use staged X error
validation; confirmed handles are released on partial failure/cancellation.
The fixture is intentionally limited to unshaped, borderless, opaque depth-24
source clients. ARGB/shape/border layout interpolation is deferred, not claimed
validated by existing fade coverage. No hidden clients are mapped for capture.

Rendering is full-frame, with no opaque occlusion shortcut, identity/nearest/no
clip restored on source picture reuse, and only compositor pictures interpolated.
Runtime samples use CLOCK_MONOTONIC elapsed time and cubic ease-out, not frame
counts. There are no geometry requests inside the rendering loop.

## Capture-ordering experiment

The fixture uses four independent X connections in one process to impose two
valid server/processing schedules deterministically:

1. Process PREPARE/capture/READY before enqueuing the target resize (positive
   control). This proves the retained-copy path can render the old image.
2. Enqueue and complete target geometry before the compositor handles PREPARE.
   Geometry no longer matches the old rectangle, so capture is skipped.

**The positive control intentionally orders test-driver execution before commit;
it is NOT a compliant implementation that makes the WM wait for READY.** These
controlled schedules are not measurements of real-process success probability.
Waiting for the compositor in the production WM to reproduce the positive
control would violate the no-delay requirement. If READY has not arrived when an
immediate arrangement commits, that effect must be bypassed. Owner mismatch,
malformed properties, stale envelopes and late READY likewise fall back instantly.

Observed normal-build metrics on private Xvfb 640x480x24:

| Metric | Result |
| --- | --- |
| Controlled early-capture transactions | 32/32 capture successfully |
| Controlled late-PREPARE transactions | 32/32 skipped safely |
| Deadline expiries among early controls | 0 in recorded run |
| Geometry notifications/client/transaction | Exactly 1 across all 64 transactions |
| Maximum geometry enqueue CPU/wall interval | 12 microseconds in recorded run; enqueue only, not server completion latency |
| Two-client capture p50 / p95 / max | 1103 / 1310 / 1642 microseconds, 35 successful captures |
| XRes baseline/final after every transaction | 11 / 11 resources; 2,457,600 / 2,457,600 pixmap bytes |

The fixture alternates position AND size changes, so it tests retained resize
rendering, not just pure moves. Each successful 160 ms visual transition uses
monotonic-clock samples; neither client gets a resize notification per frame.
Additional captures exercise generation mismatch rejection and TTL cleanup.
All copies are released after every transaction and at teardown. Pixel payload
accounting is not a claim about total driver/GPU memory or whole-server RSS.

## Input experiment — architecture counterexample

Two clients exchange their real rectangles. Red client A's retained image remains
at its old left position, while green client B's real final window occupies that
position. The test uses runtime libXtst/XTEST to inject actual server-routed input,
not XSendEvent application presses. XI2 raw observation is selected separately on
the compositor connection; no grab, interception or replay is used.

With the compositor temporarily not processing input (a permitted scheduling
order), the application receives the press immediately according to final geometry.
A root pixel at the click coordinate still shows A's red image. Only later can the
raw observer snap the presentation. The injected 25 ms compositor pause is an
adverse scheduling control, **not a measured normal input latency**.

Recorded normal-build findings:

- **1/1 ordinary clicks delivered to B while A was visibly displayed there.**
- A normal EnterNotify reached B after geometry exchange with a stationary
  pointer, while raw motion remained **0**. Cancelling on raw motion alone misses
  this crossing.
- Inject-to-observed application press: **76 microseconds** in the recorded run
  (includes test round trips, not a hardware input latency measurement).
- Inject-to-later snap: **26,560 microseconds**, including the deliberate 25 ms
  compositor processing pause.
- A second motion+button burst also reached B before the delayed observer could
  repaint. Seeing raw motion first in an event queue is not a guarantee that a
  repaint precedes a following server-delivered button.

This reproduces the known input/visual mismatch rather than fixing it. Input
continues to follow real final geometry, as it must under the semantic invariants.
It does not prove how frequently users would encounter the race, nor subjective
quality at ordinary hardware frame intervals. It DOES refute a strict guarantee
that asynchronous raw-event cancellation alone snaps before every delivery.

## Verification

| Check | Result |
| --- | --- |
| Full `make -B -j2 all` | PASS, no compiler warnings |
| `make check-layout-prototype` | PASS diagnostic assertions; explicitly NOT an integration gate PASS |
| ASan + UBSan `make check-layout-prototype` | PASS, no suppressions or reports |
| Protocol `-fanalyzer -Werror` | PASS |
| `make check-animation` | PASS, including test-only protocol/state/math cases |
| `make check-animation-xvfb check-animation-wm` | PASS existing fade/snapshot/loop/WM suites |
| `make check-swipe check-overview` | PASS headless and private Xvfb |
| `bash test/test-comp.sh` / `bash test/test-comp-present.sh` | PASS |
| Full `test/run-all.sh` | **42/42 PASS**, private UID-preserving Bubblewrap/Xvfb sandbox |
| Shell syntax / `git diff --check` | PASS; shellcheck unavailable |

Headless coverage includes 2000 state cycles, exact rectangle endpoints, cubic
bounds, long gaps, zero duration, sampled-value retargeting, prepare expiry,
backwards-time cancellation, generations, duplicate identities, payload lengths,
owner/epoch mismatches and stale/wrapped serials. The wire contract and identity
registry are not shipped or considered frozen by these tests.

Environment: Linux x86_64, GCC 16.2.1, libX11 1.8.13, XComposite 0.4.6, XRender
0.9.12, libXi 1.8.4, XPresent 1.0.0. The layout renderer uses the copy backend;
existing independent Present regressions passed, but transformed-layout Present
behavior is deferred because the input gate failed. Hardware tearing, final-frame
flashes, physical input/gesture delivery, typing/dragging during layout effects,
negative/mixed monitors, and subjective text scaling quality are **SKIP/not
validated**. No Xephyr/Xorg or active display was accessed.

Logs:

- `/var/tmp/daniwm-phase2-prototype-normal.log`
- `/var/tmp/daniwm-phase2-prototype-sanitizers.log`
- `/var/tmp/daniwm-phase2-prototype-build.log`
- `/var/tmp/daniwm-phase2-prototype-regressions.log`
- `/var/tmp/daniwm-phase2-prototype-existing-animation.log`
- `/var/tmp/daniwm-phase2-prototype-comp.log`
- `/var/tmp/daniwm-phase2-prototype-present.log`
- `/var/tmp/daniwm-phase2-prototype-full-regressions.log` — `RESULT: ALL PASS`

## Required decision before further integration

Do NOT address the mismatch by delaying logical commits, grabbing the server,
blocking/replaying ordinary clicks, adding invisible input shields, or changing
hover/focus behavior. Those violate the constraints.

Recommended next step is design review of a **more conservative effect eligibility
policy**, e.g. only animate when the affected visual/input region can be proven
non-interactive for the transition, otherwise instant bypass. A pointer-outside
check alone is not proof: the pointer may enter/click before asynchronous
cancellation is painted. A future policy needs its own prototype and explicit
acceptance criteria, and may legitimately skip most layout transitions.

No such alternative has been selected or integrated here. If the exact
snap-before-interaction guarantee cannot be met under unchanged input semantics,
layout movement effects must remain disabled/instant rather than silently weaken
the exit gate. Await review of these findings before any full Phase 2 work.
