# Animation Phase 0 — prototype and validation report

## Gate decision: PASS for Phase 0 — prototype review complete

This change provides a **test-only** lifecycle prototype and baseline assertions.
After installation of the missing test packages, all **42 existing X11 regression
suites pass** in an isolated sandbox. The snapshot prototype, real-loop tests and
headless ownership checks also pass ASan/UBSan. X-Resource verifies cleanup and
budget accounting; workspace/layout/focus/input/EWMH regressions remain green.

The engineering review below accepts the ownership split and bounded capture
experiment as Phase 0 evidence. **Phase 0 is complete; Phase 1A may be requested
next. No Phase 1 implementation has been started.** This is not approval to ship
retained-image effects or a claim of hardware rendering validation. Independent
Phase 1B/2 gates still apply before capture/geometry-transition integration.

The user installed the dependencies; the agent did not install packages. Tests
opened only newly allocated private Xvfb displays. Xephyr was not started. No
live configuration was edited, no binaries were installed and the active WM was
not restarted. Production binaries were rebuilt locally only. Existing edits in
`README.md`, `src/overview.c` and `test/test-overview.c` were preserved.

## Implemented scope

- `test/animation-snapshot.h`, `test/animation-snapshot.c`: deterministic,
  test-only resource pool with injectable capture/release callbacks.
- `test/test-animation.c`, `test/test-animation.sh`: headless identity,
  ownership, byte/count/timeout, overflow and partial-failure tests.
- `test/comp-animation.c`: includes the existing compositor implementation
  under a renamed main, following `test/comp-present.c`'s testing convention.
  Its experimental capture path copies a viewable window into an independent
  ARGB32 pixmap and Picture. It is not linked into a production executable.
- `test/test-animation-xvfb.sh`: warning-as-error compilation in copy-only and
  Present-enabled configurations, headless checks of existing fade/dim/occlusion
  calculations, then optional private Xvfb execution. It requires a newly allocated `-displayfd` display, validates the number, drops
  session environment, bounds helper runtime and never inherits `$DISPLAY`.
  Optional `xcb-res` instrumentation queries server resources through a separate
  observer connection; missing development files/extension are explicit skips.
- `test/comp-animation-loop.c`: test-only wrappers around the real compositor
  loop's `select()`/`XFlush()`. Measures fade/no-fade with copy and actual Xvfb
  Present, with independently counted measurement deadline wakeups.
- `test/animation-wm.c`, `test/test-animation-wm.sh`: simple Xlib-client baseline
  for workspace visibility, tiling, focus/EWMH, overview cancellation/ungrab,
  client removal and private compositor stop/restart. Uses fresh server event
  timestamps and synthetic action keys; no physical-input claims.
- `test/overview-xvfb.c`: narrow test-driver fix for `GetWindowAttributes`
  racing a destroyed root child during polling. Other X errors remain fatal.
- `Makefile`: separate `check-animation` (headless),
  `check-animation-xvfb` (optional private server) and `check-animation-wm`
  (private semantic baseline) targets. No startup,
  installation, launcher or production linkage changes.
- `test/test-comp.sh`: prerequisite checks, private `-displayfd` allocation,
  startup validation, isolated HOME/config/session-service commands, private
  scratch files and child-process cleanup. No guessed-display fallback.
- `test/test-comp-present.sh`: explicit missing-Xvfb skip and private display
  number validation. Existing Present lifecycle assertions are unchanged.

No animation sampler, settings, protocol, WM notification, closing effect,
workspace effect or layout interpolation has been implemented.

## Prototype ownership and resource policy

The proposed image pool uses `(identity, generation)`, not an XID alone. A stale
lookup/cancel for an old generation cannot access/remove a newer image. The
prototype's test driver supplies generations; there is **no production client
identity registry or XID-reuse detection** in this phase.

Limits are fixed experimental constants, not user settings:

| Limit | Value | Behavior |
| --- | --- | --- |
| Concurrent retained images | 8 | Refuse new capture at capacity. |
| Retained pixel payload | 33,554,432 bytes (32 MiB) | Refuse admission before any destination allocation. |
| Lifetime | 1,000 ms from capture | Explicit expiry sweeps release images; reads expire first. |
| Superseding same key | One image | Release old before allocating replacement. |
| Allocation/copy failure | No retained entry | Release any confirmed-valid partial results. |
| Completion/cancel/teardown | Explicit drop/clear | Idempotent; release Picture before pixmap. |

Size accounting uses destination dimensions, actual server pixmap-format bpp
and padded scanline stride, with checked arithmetic. It budgets **pixel payload**,
not total driver/server memory: Picture metadata, allocator/GPU padding and
transient references are not measurable from this value. Optional X-Resource
queries now validate server-owned resource counts and reported pixmap bytes.
They do not measure GPU allocation, allocator high-water marks or total Xvfb RSS.
The headless mock tracks outstanding handles, not server allocations.

Capture is staged through the compositor's existing trapped-error convention:
nonzero XIDs returned by asynchronous creation are not treated as proof of
success. Only confirmed-valid handles are published to the pool. Temporarily
named source pixmaps and source Pictures are freed after the copy; retained
images never reference a live `Win` or query the application after removal.

The retained destination is ARGB32 even for depth-24 sources. This is necessary
to clear shaped holes to transparent instead of producing opaque black corners.
Capture applies bounding shape on the **destination**, preserves full border
footprint, and restores clip state. Source Pictures are fresh, explicitly use
identity transform/nearest filter/no clip, and are freed rather than reused.
The images are never fed into the compositor's opaque occlusion shortcut.

The lifecycle experiment deliberately retains one detached image across source
resize/unmap/destroy **only for validation**. The production map/unmap/destroy
handlers remain unchanged. Pool expiry is manually driven in tests; there is no
new production wakeup timer. A future integration must arrange independent
lifetime deadlines, teardown and generation cancellation before this can ship.

The capture callback performs synchronous X error checks **inside a test
process**, never the WM. It is not an approved asynchronous capture/coordination
protocol and must not be copied into WM state-transition paths.

## Baseline findings from inspection (not runtime measurements)

- Current `win_alpha()` uses monotonic elapsed milliseconds, 160 ms default
  fade-in, map/remap restart, application opacity multiplication and generic
  override-redirect exclusion. Dim is a separate black overlay, not transparency.
- `FRAME_INTERVAL_MS` is 8 ms. The older `fading` comment saying 60 Hz does not
  describe the actual limiter. XPresent adds completion/idle gating and timeout
  fallback; 8 ms does not guarantee an actual 120 Hz presentation rate.
- Existing shape rechecks occur at 100/400 ms. Maintenance GC wakes at most
  every 5 seconds. An idle baseline must distinguish those wakeups from animation
  scheduling and account for existing debug mode's extra XSync/log overhead.
- Unmap/resize invalidates current cached images; destroy frees the tracked
  window. Fade-out cannot be achieved just by leaving `born` active.
- Tiling and fullscreen commit geometry directly in `src/layout.c`;
  `arrange()` also restacks, synchronizes and redraws the bar. Calling it each
  visual frame would violate the intended geometry/input boundaries.
- Workspace changes in `src/client.c` capture previews, unmap outgoing clients,
  arrange the target and update focus. These semantics remain untouched.
- Overview's WM-owned overlay and keyboard grab must remain independent of any
  future compositor exit image. Generic override-redirect fades stay excluded.
- `src/keys.c` still launches `--no-fade --dim 1`. There is no new config key and
  no change to the non-composited or existing instant path.

## Verification performed

Environment: Linux x86_64; GCC 16.2.1; pkg-config reports libX11 1.8.13,
XComposite 0.4.6, XRender 0.9.12 and XPresent 1.0.0 development files.

| Check | Result |
| --- | --- |
| `make -B -j2 all` with display variables unset | PASS; full local rebuild, no compiler warnings. |
| `make check-animation` | PASS; no display connection. |
| Headless tests with GCC `-fanalyzer` and `-Werror` | PASS. |
| X11 prototype with GCC `-fanalyzer` and `-Werror` | Analyzer run reports NULL `stack_cache`/`kids` at existing `src/comp.c:884`/`:901`; reviewed below as a lost relational invariant, not a demonstrated reachable NULL path. Normal warning-as-error builds and ASan runtime pass. |
| Headless tests with `-fsanitize=undefined -fsanitize-undefined-trap-on-error` | PASS. |
| ASan + runtime UBSan | PASS after user installed runtimes: headless pool, copy/Present-enabled prototype, four real-loop modes and WM test driver; no sanitizer reports. Production WM/compositor children in the WM-driver test remain normal builds. |
| `make check-swipe` | PASS; classifier, enabled/disabled adapter and settings tests. |
| `make check-overview` | Initial follow-up exposed a test-driver destroyed-child `BadWindow` race; narrow test-only fix applied, then three repeated offline/private Xvfb runs PASS. |
| `make check-animation-xvfb` | PASS builds, headless checks and private Xvfb lifecycle checks in both build modes. |
| X11 prototype with trap-mode undefined-behavior checks | PASS private Xvfb runs in both build modes, including XRes, faults and loop measurements. |
| `make check-animation-wm` | PASS with/without compositor; also PASS with driver UB traps. Three repeated normal runs pass. |
| X-Resource count/pixmap-byte assertions | PASS lifecycle baseline equality, 8-image admission, 32 MiB limit and 64 repeated post-budget cycles. |
| Injected server creation/copy errors | PASS zero-sized allocation (`BadValue`), incompatible Picture (`BadMatch`), invalid Render op (`BadValue`), each restoring resources to baseline. |
| `bash test/test-comp.sh` | PASS all seven checks after xterm installation: lifecycle, visible terminals, duplicate-owner rejection, opacity, text damage, compositor exit and instant mode. |
| `bash test/test-comp-present.sh` | PASS all seven deterministic Present buffer lifecycle checks on private Xvfb. |
| Shell syntax checks and `git diff --check` | PASS. |
| `make check-ai` (additional regression) | FAIL outside changed code: tests use removed `SourceFileLoader.load_module`; 30 shell checks pass, 5 grouped checks fail, including 2/110 Python regression cases. Not repaired in this phase. |
| Full X11 regression suite | PASS **42/42** suites; individual 100-second bounds, private Xvfb servers inside a UID-preserving Bubblewrap sandbox. Earlier UID-0 namespace attempts had PTY/environment failures and are not counted as valid WM regressions. |

Measured headless results:

- 10,000 successful create/drop lifecycle cycles after partial-failure tests.
- Count admission reached exactly 8; byte admission reached exactly 32 MiB.
- Zero outstanding fake pixmap/Picture handles and zero charged bytes after
  every completed test cycle.
- Exact TTL boundary, overflow, padded stride and stale-generation cancellation
  assertions passed. These are deterministic model results, **not X resource
  leak or real rendering results**.
- Existing compositor arithmetic passed headlessly in both build modes: fade at
  0/80/160 ms, application-opacity multiplication, zero duration, disabled fade,
  override exclusion, ARGB/shaped occlusion exclusions, inactive/active/fullscreen
  dim policy. These invoke existing functions with injected sample times, not a
  running animation/render loop.

## Private-display results and remaining metrics

Verified installed packages: `xorg-x11-server-Xvfb` and
`xorg-x11-server-Xephyr`, both `21.1.26-1.fc45.x86_64`;
`xterm-411-1.fc45.x86_64`, `libasan-16.2.1-3.fc45.x86_64` and
`libubsan-16.2.1-3.fc45.x86_64`.
The following commands were rerun with inherited display variables unset:

```sh
make check-animation
make check-animation-xvfb
make check-animation-wm
make check-swipe check-overview
bash test/test-comp.sh
bash test/test-comp-present.sh
```

The prototype checks existing fade/dim/occlusion calculations headlessly. Its
following X11 runtime checks now pass:

1. Existing rendering of an opaque source and correctness of the back buffer.
2. Thirty-two forced copy-backend full repaints at 640×480, printing mean,
   p50/p95/max millisecond duration. This is a synchronized micro-workload,
   not an event-loop, refresh-rate or input-latency benchmark.
3. One thousand settled `repaint_due()` queries without animation paint. This
   is not proof of event-loop idle wakeup behavior.
4. Border/content survival after resize/unmap/destroy, rejection of hidden/dead
   capture, shape transparency over 128 lifecycle cycles, and ARGB preservation
   when the server supplies that visual. Missing ARGB coverage is an explicit SKIP.
5. Created/released owned-image counts and peak pixel bytes. Count equality
   alone does not establish actual server-side memory reclamation.

Observed normal-build Xvfb results (640×480, both runs use the copy backend):

| Metric | Copy-only build | Present-enabled build |
| --- | --- | --- |
| Forced full repaints | 32 | 32 |
| Mean synchronized repaint time | 0.312 ms | 0.312 ms |
| p50 / p95 / max | 0 / 1 / 4 ms | 0 / 1 / 3 ms |
| Owned images created / released | 130 / 130 | 130 / 130 |
| Peak retained count / charged pixel payload | 1 / 101,184 bytes | 1 / 101,184 bytes |

Each lifecycle run covers border/content survival across resize/unmap/destroy,
hidden/dead capture rejection, 128 shaped cycles and ARGB preservation. A trapped
`BadWindow` is expected from the deliberately destroyed-source capture test.
Undefined-behavior trap builds passed the same runtime checks; observed mean
repaint times were 0.281/0.312 ms. These millisecond-resolution micro-workload
numbers are not real animation frame intervals or hardware latency guarantees.
Image-release counts track confirmed-owned client handles, not X server memory.
The existing Present regression simulates completion/idle events; it does not
establish real GPU pacing or hardware-vsync behavior.

## Follow-up: server resources, event loop and semantic baseline

The earlier 130-image results above describe the initial prototype. The expanded
prototype now creates/releases **205/205 retained images** per build. It reaches
both configured limits: **8 images and 33,554,432 charged pixel bytes**.
X-Resource reports the test compositor client returning to **8 resources and
2,457,600 pixmap bytes**, identical to its pre-lifecycle baseline. The baseline
includes compositor back/background buffers; it is not zero and is not the
whole server's memory. Filling the snapshot byte budget adds exactly **2 resources
and 33,554,432 reported pixmap bytes**. Sixty-four subsequent create/clear cycles
are individually checked against the same server-resource baseline.

The expanded runtime checks cover actual asynchronous errors at destination
pixmap creation, destination Picture creation, and Render copy. Each error is
intentional and logged by the existing trap; partial allocations are released,
the pool remains empty, and X-Resource values return to baseline. A real full-byte
snapshot expires at the configured TTL, and further admissions are rejected
before allocation when count or byte capacity is exhausted. Real driver-side
`BadAlloc` due to memory exhaustion is not induced; safely invalid requests and
mock allocation failures cover the error paths without exhausting the server.

The real-loop test calls existing `comp_main()` in a test binary with only
`select`/`XFlush` observation wrappers. It lets the compositor settle for 800 ms,
then observes 1,500 ms. Two measurement deadline wakeups are separate from the
compositor's own timers; debug mode is disabled. Latest normal-build results:

| Mode/backend | Paints including startup | Paint-submit p50 / p95 | Settled paints / events / timers | Settled CPU including teardown |
| --- | --- | --- | --- | --- |
| Fade/copy | 21 | 7.965 / 8.825 ms | 0 / 0 / 0 | 0.349 ms |
| Fade/Xvfb Present | 11 | 16.430 / 17.344 ms | 0 / 0 / 0 | 0.320 ms |
| Instant/copy | 2 | One startup interval only | 0 / 0 / 0 | 0.305 ms |
| Instant/Xvfb Present | 2 | One startup interval only | 0 / 0 / 0 | 0.307 ms |

These intervals include startup/shape rechecks, measure submission rather than
scanout and use an instrumented software X server workload. They are not latency
or hardware-vsync guarantees. The 1.5-second idle window deliberately excludes
initial shape checks and the existing 5-second GC deadline; it does not imply
that production has no periodic maintenance wakeup. Trap-mode UB builds passed
all four modes as well. No production wakeup or clock changes were made.

`check-animation-wm` passes without a compositor and with a private dani-comp
child that is stopped/restarted. It checks tiled rectangles, per-client desktop
properties, hidden outgoing windows, return-to-workspace focus and EWMH agreement,
non-committing overview browsing, Escape cancellation, keyboard-grab release and
focus recovery after client destruction. It tests controlled stop/restart, not
selection theft by another compositor. The overview driver previously used Xlib's
default fatal error handler while scanning stale root children; the test-only
handler now tolerates only the specific destroyed-child attribute-query race.
Fresh server timestamps are used for synthetic action keys; fabricated CurrentTime
key events did not model real focus ordering reliably. Three repeated normal
semantic/overview runs passed after the driver fixes.

Some semantic runs emit existing compositor trapped `BadDrawable`/`BadMatch`
messages during rapid WM hide/overview teardown. The process survives and the
semantic assertions pass, but these races have not been fully diagnosed and are
not claimed resolved. No new retained-image code is linked into those processes.

## Final validation after dependency installation

All 42 suites listed in `test/run-all.sh` were executed individually with
`timeout -k 3 100`, using the existing normal binaries and built test helpers.
**42 passed, 0 failed.** This includes verify, config, kill/mouse/workspaces,
struts/RandR/tray/restart/tasklist, compositor/Present, nested/overlapping floats,
click delivery/raising, focus stealing/model/time, hidden/remapped clients,
ICCCM input/WM state, X errors, urgency, workspace storm, reload, fullscreen,
volume hangs, gap bounds, rename, tiled restacking, scratchpad, transients and
launcher tests. No AI tests were rerun; their previously reported Python import
compatibility failure is outside the C/X11 animation scope.

Regression log for this session:
`/var/tmp/daniwm-phase0-regressions-final.CTS6gw`.
The sandbox used `env -i` with `TERM=xterm`, `SHELL=/bin/bash`, no inherited
display/session environment, a read-only host root, writable repository bind,
private `/dev`, `/proc`, `/tmp`, `/run`, `/var/tmp` and masked user home. All
namespaces were unshared with `bwrap --unshare-all --die-with-parent`. Session
import commands were replaced with no-op scripts on the sandbox PATH. PID and
network isolation plus private X socket storage prevent old fixed-display test
scripts from reaching the active desktop.

The initial `unshare` runs mapped the test user to UID 0 and were invalid for
terminal-dependent tests: xterm first failed PTY group ownership, and the second
attempt still lost terminal clients in that environment. These failures were
investigated, not suppressed. Preserving UID 1000 and providing Bubblewrap's
private `/dev` fixed the environment; smoke tests passed before rerunning all
42 suites. No test assertions were weakened to obtain the final result.

The following sanitizer command pattern now succeeds, without disabling leak
detection or adding sanitizer suppressions:

```sh
env -u DISPLAY -u WAYLAND_DISPLAY \
  CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  make check-animation check-animation-xvfb check-animation-wm
```

`check-swipe` and offline/private-Xvfb `check-overview` also passed again.
`git diff --check` is clean. No production animation code, config, launcher
flags or installation behavior was added.

## Engineering review and scope decision

1. **Ownership:** retain an independent compositor-owned ARGB image, never a
   `Client *` or pointer into live `Win` state. The experiment demonstrates
   survival across source resize/unmap/destroy. Keep WM geometry, focus, input,
   map/unmap, workspace and EWMH operations authoritative and immediate.
2. **Limits:** 8 images / 32 MiB payload / 1 second are accepted experimental
   ceilings, not mandatory user-facing defaults or a promise to animate every
   client. Reject/skip over-budget captures. Real driver memory can exceed
   payload accounting. A full 4096×2048 ARGB image fills the byte budget alone;
   later phases must retain graceful instant fallback on large/multi-monitor
   workloads. Do not pool full-screen snapshots in the overview thumbnail cache.
3. **Capture:** staged asynchronous-error validation and destination shape
   clipping are supported by the lifecycle tests. The synchronous test callback
   is **not** approved for use in WM event handlers. Generation assignment,
   scheduled expiry, capture freshness and lifecycle reasons must be designed
   at Phase 1B before real retained-image effects are integrated.
4. **Cross-connection ordering:** same-connection request order does not prove
   another process captured before unmap/resize. No capture protocol has been
   validated or authorized by this experiment. Later coordination must be
   asynchronous, bounded and optional; missing content means skip, never stall.
5. **Input tradeoff:** compositor-side geometry animation is the preferred
   hypothesis to avoid client resize storms. Real input still uses final window
   geometry. Phase 2 must prototype interaction/cancellation before adopting it;
   no invisible shields, grabs or delayed WM state transitions are approved.
6. **Analyzer finding:** the existing code initializes `stack_cache=NULL` and
   `stack_cap=0`; its only later writes publish a successful non-NULL realloc
   pointer before publishing positive capacity. Allocation failure returns
   before memcpy, and this file never frees/clears the pointer independently.
   Thus `stack_cap>0 => stack_cache!=NULL`; `nk>0 && nk<=stack_cap` cannot yield
   NULL in the reported memcpy path under the current single-threaded code.
   The analyzer lost this relational invariant. Its diagnostic remains recorded;
   no production edit is needed to silence it in a test-only phase.
7. **Existing X races:** historical rapid-hide/overview runs emitted trapped
   BadDrawable/BadMatch around live content acquisition; all semantic and full
   regression checks now pass. Live NameWindowPixmap/Picture creation can race
   unmap/destruction across connections, so failed capture must be skipped.
   The exact historical request is not proven from the old log (which lacks
   request/resource IDs); these are not claimed fixed or definitively harmless
   visually. They are tracked baseline limitations, not evidence against the
   isolated prototype's measured resource ownership. Diagnose/reproduce with
   richer test-only instrumentation if they recur when capture effects ship.

**Phase 0 ends here.** Later-phase adversarial race soaks, compositor-selection
loss handling and visual/hardware validation are tracked at their own integration
and release gates; they are not an open-ended extension of this baseline phase.
Xvfb does not prove tearing, GPU pacing or physical touchpad delivery. Obtain
separate approval for any live/hardware-backed session test. The next scoped
implementation, if requested, is **Phase 1A only: configurable existing fade-in
and reduced motion**, with no snapshot effects or geometry transitions.
