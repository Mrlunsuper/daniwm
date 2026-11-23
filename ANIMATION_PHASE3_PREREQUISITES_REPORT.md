# Phase 3 — prerequisite assessment only

## Decision: not ready for implementation

Authorized scope was **assessment only**, with Phase 2 still blocked. This file
is the only repository change made for this assessment. No runtime effect,
setting, launcher flag, production protocol, test fixture or roadmap dependency
was added or changed. Existing uncommitted work was preserved.

**Technical independence:** opacity-only workspace crossfades need not depend on
client rectangle interpolation. They can leave incoming client geometry and all
WM logical transitions immediate.

**Gate independence:** this does NOT authorize skipping Phase 2. The current
`ANIMATION_PLAN.md` requires each phase's exit gate before the next phase starts,
and Phase 3 explicitly requires an approved capture/coordination path. Phase 2
has not passed and no capture protocol has been approved. An explicit roadmap
review/deferral decision is needed before even a separately scoped Phase 3
prototype is treated as the next delivery phase. This assessment does not make
that decision or mark either phase complete.

Even with an approved dependency change, workspace capture, cancellation, input
and monitor/resource prototypes remain prerequisites. Production integration is
therefore **BLOCKED**, not merely awaiting a duration setting.

## Current implementation evidence

### WM state and ordering

`src/client.c:view()` currently:

1. Hides overview and attempts bounded overview thumbnails for current clients.
2. Validates the requested workspace.
3. Saves selection, updates prevws/curws and chooses the incoming selection.
4. Publishes desktop properties via `ewmh_desktops()`.
5. Unmaps outgoing clients through `animation_unmap()`.
6. Arranges/maps the incoming workspace, then applies focus or root focus.

This semantic ordering must remain authoritative. Capture readiness must never
hold curws, focus, unmapping, EWMH publication or arrange completion. Existing
thumbnail capture calls are not permission to add synchronous full-size capture
on the WM connection.

Other entry points need explicit treatment, not broad inference from any unmap:

- `send_to()` changes the selected client's desktop and follows it. The sent
  client is not in the outgoing workspace unmap loop after its reassignment;
  avoid duplicating it in outgoing and incoming visuals.
- `move_to()` handles external pager moves without following. It can hide one
  client without changing the current workspace; this is not a workspace switch.
- `ws_toggle()`, key bindings, bar wheel/click, `_NET_CURRENT_DESKTOP`, permitted
  activation requests and completed swipe actions converge on workspace actions.
  Their triggering semantics and gesture delivery must remain unchanged.
- Startup/restart adoption and `config.c` workspace folding can change desktop
  state outside an ordinary user switch. Treat reload/restart as cancellation/
  bypass, not inferred workspace animation.
- Monocle hides, scratchpad parking, client close and overview teardown remain
  distinct lifecycle reasons. No hidden client may be mapped to build a layer.

`src/layout.c` already owns per-monitor layout, fullscreen geometry and stacking.
Phase 3 should use its instant geometry path, not implement Phase 2 by stealth.
Docks, bar, tray and appropriately stacked UI must remain stationary and must not
be burned into a fading application layer.

### Existing compositor channels are not the needed protocol

`src/keys.c` and `src/animation-lifecycle.h` publish fade settings and ordered
per-client hide reasons. The reason precedes the WM's unmap request on the same
connection. It does **not** prove another connection captured content first.
The payload contains no workspace transaction, old/new workspace identities,
monitor epoch, complete member set, capture readiness or transaction cancellation.

`src/comp.c` currently:

- Suppresses closing-image detachment when a hide reason is attached.
- Cancels detached close layers on valid hide messages and desktop property
  notifications; map/unmap handling preserves instant workspace transitions.
- Invalidates live pictures on resize and frees them on unmap.
- Supports per-window closing layers, not full-resolution workspace composition.

Reusing those messages as capture requests or deleting their cancellation to
make workspace fades appear would violate existing lifecycle policy. The narrow
settings/control capability is not a claim of workspace-animation support.

`test/layout-prototype.h` provides **test-only** bounded batch, owner/epoch,
sequence, generation and PREPARE/READY/COMMIT concepts. Its atoms are explicitly
experimental. Its early-capture positive control orders test-driver execution
before commit; it is not proof of an asynchronous no-delay WM integration.
Do not promote it to a frozen production protocol or carry forward its trial
16 ms deadline as a WM wait.

### Snapshot ownership and candidate capture approaches

Reusable evidence exists, but not a workspace solution:

- Phase 0's test-only copied ARGB images survive resize/unmap/destroy and validate
  partial-failure cleanup, shape/alpha and byte/count/TTL limits.
- Phase 1B transfers already-painted named pixmap/Picture references for closes,
  keyed by client identity/generation. That proves bounded resource lifetime,
  not an atomic, correctly ordered full-workspace image.
- Overview thumbnails are WM-owned, limited to 64 entries of at most 192x108,
  and captured only while viewable. They must not be repurposed as animation
  buffers or replaced with live hidden-app previews (`src/overview.c`).

Possible **future prototype hypotheses**, not selected implementations:

1. Retain an already-confirmed compositor-owned outgoing scene or a validated
   set of already-painted window images before their ordinary live references
   are invalidated. Prove freshness, member completeness, stacking, budget and
   scene identity under event ordering; named pixmaps are not automatically
   immutable copies while sources remain mapped.
2. Opportunistic asynchronous prepare/readiness: commit the WM switch regardless
   of readiness; admit an effect only if a matching safe image already exists.
   Late readiness must discard/bypass, never delay or revive an old switch.

Neither hypothesis currently has workspace-level tests. Copying an outgoing
image on demand after a PREPARE notification may be too late. Retaining all
possible hidden workspaces, duplicating the overview cache, mapping hidden apps,
or waiting in the WM are not acceptable substitutes.

## Input prerequisite: distinguish semantics from visual identity

Crossfade has no moving hit-test rectangle for incoming windows. That avoids the
specific transformed-local-coordinate problem from Phase 2, but does not settle
input acceptance. An outgoing image may dominate a pixel while the server routes
pointer input and typing solely to the incoming workspace.

The Phase 3 semantic gate requires outgoing clients to be unmapped/unfocused and
receive no input. The test must also expose and review what is visibly under a
click: correct incoming delivery can coexist with misleading outgoing pixels.
Phase 2 raw-event cancellation cannot be assumed to guarantee snap-before-input.
Input-transparent pictures prevent interception, not asynchronous visual lag.

This is a **design risk inferred from inspection**, not a newly measured
workspace-crossfade failure. Existing fade approval does not itself specify
workspace ghosting/hit-identity acceptance. Conversely, the failed moving-window
prototype does not prove every opacity-only workspace effect impossible.

A dedicated private-server prototype must compare instant baseline and crossfade
for stationary hover, click/wheel, typing, rapid switches and direct interaction,
including delayed compositor scheduling. Record actual receiving client/focus/
EWMH and displayed pixels independently. Do not solve discrepancies with input
shields, grabs/replay, keeping outgoing clients mapped, delayed focus or altered
hover rules. If the required visual/input property cannot be met, skip the effect.

## Resource, rendering and monitor prerequisites

Existing close-image policy is 8 images / 33,554,432 pixel bytes / 1000 ms. It is
useful conservative precedent, not authorization to allocate another independent
unaccounted pool. A workspace design needs an explicit aggregate budget covering
close layers, outgoing scenes, retained members and in-flight flattening/copies.
Account by actual server bpp/padded stride and dimensions, with overflow checks.
Normal rendering/Present buffers remain distinct resources, not free scratch
storage for retained scenes.

Illustrative ARGB32 payload arithmetic, **not measured allocations**:

| Dimensions | One image | Two such images |
| --- | ---: | ---: |
| 1920x1080 | 8,294,400 bytes | 16,588,800 bytes |
| 2560x1440 | 14,745,600 bytes | 29,491,200 bytes |
| 3840x2160 | 33,177,600 bytes | 66,355,200 bytes |
| 7680x2160 | 66,355,200 bytes | 132,710,400 bytes |

One 4K image barely fits 32 MiB; a second does not. Two side-by-side 4K outputs
exceed it even for a single bounding-box image. Monitor gaps can further waste
bounding-box allocation. Admission must permit instant fallback, not silently
increase limits, evict live client state or create an unbounded switch queue.

Rendering prerequisites:

- Per-monitor clipping, negative coordinates, unequal outputs, holes/overlaps,
  empty desktops, fullscreen and scratchpad eligibility.
- Preserve application opacity, ARGB and shapes; compose application content,
  shadows/background and stationary UI with explicitly reviewed semantics.
- Exclude temporary/translucent layers from opaque shortcuts; initially full
  repaint, then only optimize with old/new extents and shadow correctness tests.
- Reset source transforms/filters/clipping and destination clipping on reuse.
- Latest logical workspace wins. Either discard old layers or boundedly flatten;
  count the simultaneous source/destination payload during flattening.
- Monotonic duration, exact final frame, no animation wakeups after cleanup, and
  independent expiry despite Present stalls/event storms.

`src/monitor.c` uses Xinerama geometry and WM RandR notifications, including
changes that need not change the root's total size. The compositor currently
cleans up on root ConfigureNotify but has no complete workspace monitor registry/
layout epoch. Root resize alone is not a sufficient future topology-cancellation
signal. Monitor/owner/reload/restart epochs must reject stale layers even when
workspace indices or XIDs are reused.

## Prerequisite matrix

| Requirement | Evidence now | Assessment |
| --- | --- | --- |
| Explicit phase dependency approval | Phase 2 blocked; sequential roadmap unchanged | Missing decision |
| Immediate WM semantics | Existing instant paths and regressions | Preserve, not refactor for timing |
| Owned retained X resources | Phase 0/1B evidence | Reusable building block only |
| Workspace capture before ordinary unmap | No approved path | Missing prototype |
| Production capability/owner/sequence/cancel protocol | Hide/settings channels only | Missing specification + validation |
| Workspace generation/member/monitor identity | No scene registry/epoch | Missing design |
| Opacity-only input/visual acceptance | Layout counterexamples only | Missing dedicated review/prototype |
| Stationary UI and per-monitor composition | Normal layout/render paths only | Missing workspace renderer tests |
| Coalescing and aggregate budgets | Bounded per-window close pool | Missing scene-level policy/tests |
| Fallback/replacement/expiry correctness | Existing fade tests | Missing workspace failure/storm tests |
| Hardware pacing/appearance | No workspace implementation tested | Unavailable/unverified |

## Proposed validation sequence if separately authorized

These are future gates, not work performed by this assessment:

1. Review explicit deferral/dependency changes while retaining Phase 2 BLOCKED.
   Do not relabel it complete or weaken ownership/input invariants.
2. Specify workspace transaction identity, bounded payloads, capability/owner,
   cancellation epochs, readiness and immediate-bypass semantics. Prototype
   adversarial ordering and stale/overwritten messages before freezing any API.
3. Demonstrate outgoing capture freshness/completeness and no hidden mapping,
   shared resource admission and cleanup under X errors, close/unmap/destroy,
   timeout, owner replacement and rapid A→B→C→A switches.
4. Validate crossfade input/visual policy independently, including delayed
   compositor processing. Pause with findings before runtime integration.
5. Prove per-monitor composition and stationary UI, Present/copy correctness,
   exact final content and no post-effect wakeups. Only after required prototype
   gates pass consider full integration and that phase's setting/documentation.

The current request authorizes none of these prototypes or runtime changes.

## Baseline verification performed in this assessment

Only existing tests were run; no workspace-crossfade metrics were collected.

- `make all check-animation check-swipe check-overview check-animation-wm`:
  PASS; offline checks and newly allocated private Xvfb semantic baselines.
- Eight selected existing suites: workspaces, workspace storm, hidden reload,
  off-workspace fullscreen, scratchpad focus, remap focus, RandR and restart:
  **8/8 PASS** inside a UID-preserving Bubblewrap sandbox with private Xvfb,
  sockets/devices/session environment. The 50-flip storm checks client-list
  stability, not animated visual completeness or latency.
- `git diff --check`: checked during inspection; no code edits were made.

Logs:

- `/var/tmp/daniwm-phase3-prerequisites-baseline.log`
- `/var/tmp/daniwm-phase3-prerequisites-workspaces.log`

The manual-compositor semantic baseline logged one existing trapped BadMatch;
its assertions passed. This is not claimed fixed or newly caused by documentation.
No full rebuild, full 42-suite rerun, sanitizer run or workspace-specific benchmark
was needed or claimed for this documentation-only assessment. Historical reports
contain earlier runs but do not substitute for missing workspace prototypes.

## Disposition

Workspace crossfade is separable from movement **as a rendering concept**, not
ready as an implementation or automatically exempt from phase discipline. Keep
workspace transitions instant. Further action requires an explicit dependency/
roadmap decision and separately scoped protocol/capture/input prototypes. No
`animation_workspace_ms`, directional slide, overview effect or live-session
change is warranted by this assessment.
