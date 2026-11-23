# Animation roadmap dependency revision — proposal for review

**Status: APPLIED AS A DOCUMENTATION-ONLY ROADMAP AMENDMENT.** This document
does not pass an exit gate or authorize a prototype, runtime feature,
configuration key, installation or live-session change.

The amendment keeps the original strict Phase 2 gate explicitly unmet while
allowing its separately labeled best-effort implementation, and adds separately
gated Phase 3 prototype stages. Existing user work remains intact.

## 1. Decision requested

Approve a narrow exception to the roadmap's sequential dependency:

> Keep Phase 2 layout/discrete movement explicitly BLOCKED and DEFERRED, with its
> original exit gate unchanged. Allow separately requested, independently gated
> Phase 3 workspace-crossfade prototypes after verifying Phase 0/1A/1B evidence.
> Do not require a passing layout-movement implementation for those prototypes.
> Require workspace-specific capture/protocol, input and resource/rendering gates
> before considering a separately requested Phase 3 production integration.

The applied amendment permits a **future, separately authorized roadmap branch**,
not immediate prototype coding. Each subsequent request still selects one
phase/substage; an agent must inspect, plan, test, pause at its prototype gate and
report findings. A generic “next phase” must not be interpreted as passing a
failed gate.

No Phase 3 prototype or implementation is authorized by this documentation change.

## 2. Rationale and evidence

The following reports are evidence, not dependencies silently marked complete:

- `ANIMATION_PHASE0_REPORT.md`: bounded retained-image lifetime and baseline
  measurements; the copied-image experiment is test-only.
- `ANIMATION_PHASE1A_REPORT.md` and `ANIMATION_PHASE1B_REPORT.md`: privately
  verified fade behavior, lifecycle reasons, resource limits and instant paths.
  Their reported passes do not imply hardware validation or installed releases.
- `ANIMATION_PHASE2_PROTOTYPE_REPORT.md`: asynchronous raw input observation
  cannot guarantee repaint before an application receives a click at its final
  real geometry. PREPARE-before-resize does not guarantee capture ordering.
- `ANIMATION_PHASE2_ELIGIBILITY_REPORT.md`: sampled pointer position, input
  transparency, occlusion and input shapes establish no safe movement lifetime.
  Fail-closed instant behavior is not a completed movement animation.
- `ANIMATION_PHASE3_PREREQUISITES_REPORT.md`: opacity-only workspace rendering
  need not transform incoming client hit-test rectangles, but has independent
  capture, input/visual, monitor and resource prerequisites that are unvalidated.

Thus there is a reasonable basis to **investigate** workspace crossfade without
shipping layout movement. There is no basis to claim workspace crossfade safe or
to promote the test-only layout protocol to approved API. This proposal changes
one dependency, not the standard of proof or the semantic invariants.

## 3. Proposed dependency graph and status

```text
Phase 0 -> Phase 1A -> Phase 1B
                         |
                         +-> Phase 2: BLOCKED / DEFERRED
                         |     resume only with a genuinely different,
                         |     compliant design and its own passing gate
                         |
                         +-> Phase 3-P1: capture/protocol prototype [new request]
                               |
                               v  pause, report, explicit review
                             Phase 3-P2: input/render/resource prototype [new request]
                               |
                               v  pause, report, explicit review
                             Phase 3-I: production integration [new request]
                               |
                               v  original Phase 3 exit gate + validated prerequisites
                             Phase 4: remains a separate future phase
```

`3-P1`, `3-P2` and `3-I` are proposed review labels, not new runtime features or
permission to combine phases. Phase numbers stay stable; Phase 2 is not renamed,
removed, declared successful, or implemented indirectly through workspace work.

| Work | Proposed status after an approved amendment |
| --- | --- |
| Phase 0/1A/1B | Existing reported evidence; verify relevant baselines before new work |
| Phase 2 | Best-effort opt-in shipped; strict input-safe gate remains unmet; direct manipulation remains instant |
| Phase 3 prototypes | May be separately requested; not pre-approved or passed |
| Phase 3 integration | Blocked until all relevant prototype gates and reviews pass |
| Phase 4 | No changed dependency or authorization; separate request after Phase 3 gate |

A blocked Phase 3 prototype would stop this branch too. It would not justify
advancing to Phase 4 or shipping a partial effect with an unmet safety gate.

## 4. Constraints carried forward without exception

- WM logical geometry, mapping, stacking, focus, workspace selection, input and
  EWMH semantics remain authoritative and immediate. No readiness acknowledgment
  may delay an unmap, removal, desktop/focus update or final geometry commit.
- The compositor alone owns interpolation and retained visual layers. No
  per-frame real client resizing, WM animation timer or animation server grab.
- No ordinary-input blocking, replay or new animation grabs; no invisible input
  shields, changed hover/focus rules, or keeping outgoing clients mapped to
  support a visual effect. Existing semantic input mechanisms are not repurposed.
- Disabled compositor/motion, reduced motion, zero duration and unavailable or
  incompatible compositor use the instant path without animation work/timers.
- Missing, late, stale, oversized or unsafe capture skips the effect, not the
  state transition. Application opacity properties are not overwritten.
- No hidden application mapping/capture, hidden live previews, thumbnail-cache
  repurposing, slide, overview polish, spring physics, blur or finger-follow
  gestures. Workspace work starts with fixed opacity-only crossfade hypotheses.
- Monotonic-time sampling, identity plus generation, bounded validated payloads,
  strict aggregate resource admission and complete X-resource cleanup remain
  mandatory. Translucent/transformed layers cannot use opaque shortcuts;
  transforms, filters and clipping must be reset before picture reuse.
- Tests use isolated displays. Hardware checks require explicit approval;
  unavailable validation is SKIP/BLOCKED. No installation, live config changes or
  active WM restart without separate permission.

The 8-image/32 MiB/1000 ms retained-image policy is existing conservative evidence,
not permission for an additional unaccounted workspace pool. Prototypes must
specify an aggregate budget including in-flight copies/flattening. Any proposed
budget change is an explicit review item, never an incidental phase bypass.

## 5. Proposed prototype and integration gates

### 3-P1 — workspace capture and protocol, test-only

**Entry:** approved dependency amendment, separate explicit request, relevant
Phase 0/1A/1B baseline verification and an inspect/plan statement.

**Scope:** choose and test a candidate outgoing-image strategy; define a small
experimental workspace transaction with owner/capability, owner/monitor epochs,
workspace/member identities and generations, bounded sequence/payload validation,
cancellation, expiry and immediate bypass. Retain already-safe content or use
opportunistic readiness; do not force success by waiting in the WM.

**Required evidence:**

- Capture freshness, stacking/member completeness and ownership while sources
  are viewable; no mapping or querying hidden apps to obtain missing content.
- Independent-connection adverse schedules, including PREPARE processed after
  unmap, late READY, overwritten properties, stale generations, owner replacement
  and timeout. Deliberately ordered positive controls are labeled as such.
- Logical workspace/focus/EWMH/unmap ordering and timing do not depend on capture
  success. Instrumentation distinguishes enqueue time from server completion.
- Resource admission before allocation; cleanup on partial X errors, cancellation,
  expiry and teardown; count/bytes return to baseline over repeated cycles.

**Exit:** report metrics and failure/bypass cases; pause for explicit review.
Only a reviewed passing prototype can establish an approved coordination path.
No production atoms, settings or protocol freeze before that review. A prototype
that captures nothing safely is a valid negative result, not a feature gate PASS.

### 3-P2 — workspace input, rendering and resource behavior, test-only

**Entry:** separate request and reviewed passing 3-P1; no automatic continuation.

**Scope:** opacity-only workspace pictures at fixed rectangles, bounded newest-
workspace-wins cancellation, stationary UI and per-monitor composition. Preserve
instant WM layout; do not introduce layout transitions or general geometry IPC.

**Required evidence:**

- Incoming focus, EWMH and input delivery agree; outgoing clients remain unmapped
  and receive no input. Record receiving clients and displayed pixels separately
  for click/wheel, typing, stationary hover and delayed compositor processing.
- Review visible outgoing/incoming ambiguity explicitly. Correct logical delivery
  alone does not certify an acceptable visual/input policy. No unapproved
  latency/probability tolerance, or raw-event snap-before-delivery assumption.
  If acceptance cannot be met without semantic changes, remain instant/blocked.
- A→B→C→A storms, close/unmap/destroy and scratchpad/fullscreen interactions;
  newest logical workspace wins with no unbounded queue or hidden captures.
- Per-monitor clipping including negative/unequal geometry, topology changes even
  without root resize, empty workspaces, bar/dock/tray stacking, ARGB/shape/
  opacity and exact final frame. No stationary UI baked into the fading layer.
- Aggregate budget/exhaustion and in-flight resource accounting, malformed/stale
  transactions, reload/owner/monitor cancellation, Present/copy fallback, no
  residual images or animation-driven wakeups after completion or cancellation.

**Exit:** report, review and pause before production integration. Required
unavailable checks remain open, not silently passed. Any hardware-dependent
claim needs an explicitly approved isolated hardware-backed test.

### 3-I — production integration and release gate

**Entry:** separately requested integration after all required prototype findings
are reviewed and passed; validated protocol/ownership/input design is documented.

**Scope:** integrate only Phase 3, with current WM state ordering unchanged. Add
only its completed-phase setting/documentation/launcher support, never deferred
Phase 2 or Phase 4 keys. Keep opt-in and instant fallback.

**Exit:** preserve the original Phase 3 exit gate verbatim, supplemented by the
validated prototype obligations:

> Workspace-storm, focus, hidden-client, fullscreen and scratchpad regressions
> pass; no input remains assigned to an outgoing workspace and no outgoing image
> survives cancellation.

Build, headless animation/protocol/accounting tests, private Xvfb compositor/
Present and WM integration, `check-swipe`, `check-overview`, regression and
performance/cleanup gates still apply. Do not advertise unverified hardware
quality, install or restart a live session as part of completion.

## 6. Amendment applied

The narrow amendment was applied to `ANIMATION_PLAN.md` as documentation only:

1. The opening status records the dependency revision while preserving
   proposal-only/no-runtime-change status.
2. The architecture and coordination sections identify Phase 2 as
   BLOCKED/DEFERRED and distinguish the independent Phase 3 workspace branch.
3. Phase 2 retains its original feature scope and exit gate; no movement feature
   was marked complete.
4. Phase 3 names the 3-P1 capture/protocol and 3-P2 input/render/resource
   prototype gates before separately requested 3-I integration.
5. Release order keeps Phase 2 deferred and does not authorize Phase 3 prototypes,
   runtime settings, README/config changes or Phase 4.

The roadmap's implementation survey is historical and partly predates shipped
fade code. Status clarification cites current phase reports rather than rewriting
old reports or expanding this amendment into unrelated cleanup.
Example configuration, README and runtime launcher settings remain unchanged
until the corresponding feature is actually completed.

The following original amendment details remain the governing intent:

1. **Opening status/phase-discipline paragraph:** explicit dependency gates name
   only the approved Phase 2 deferral and independently gated Phase 3 branch.
2. **Architecture recommendation:** annotate compositor-side layout movement as
   an unapproved hypothesis blocked by the two Phase 2 reports. Keep the original
   input gate; do not present asynchronous snap/cancellation as a proven fix.
3. **Coordination section:** retain required protocol/capture validation and
   no-delay rules. Explain that the future workspace prototype must pass these
   independently; existing hide/settings messages and test layout atoms are not
   the approved path.
4. **Phase 2 heading/status:** add BLOCKED/DEFERRED plus evidence links. Preserve
   its feature scope and original exit gate so resumption requires genuine proof.
5. **Phase 3 entry:** name Phase 0/1A/1B evidence and separate 3-P1/3-P2/3-I
   request/review gates; retain the original Phase 3 scope and exit gate.
6. **Release order:** show the explicit branch rather than implying layout has
   shipped. Do not change Phase 4 dependencies or add new feature promises.

The roadmap's implementation survey is historical and partly predates shipped
fade code. Any status clarification should cite current phase reports rather than
rewrite old reports or expand this dependency amendment into unrelated cleanup.
Example configuration, README and runtime launcher settings stay unchanged until
the corresponding feature phase is actually completed.

## 7. Approval boundaries and next action

The amendment establishes three separate future decisions; none is inferred from
another:

1. The dependency revision is applied while preserving the original Phase 2
   strict gate as unmet; best-effort movement is separately labeled and opt-in.
2. Separately request the single 3-P1 test-only prototype and review its plan.
3. Separately request 3-P2 and then 3-I only after each preceding review passes.

Approval of a prototype result is likewise not permission for the next prototype,
full integration, hardware testing or deployment. A requested scope can be
rejected or narrowed if inspection shows unresolved prerequisites.

The resulting policy keeps the strict Phase 2 input guarantee unmet and keeps
workspace changes instant until their independent gates pass. It does not claim
the existing input blocker is solved; the shipped best-effort movement effect is
the explicitly accepted exception and is disabled by default.

## 8. Verification of this amendment

- Read the authoritative roadmap and the Phase 3 prerequisite assessment;
  cross-checked the recorded Phase 1B/Phase 2 gate statuses.
- `git diff --check` passed after the amendment.
- `ANIMATION_PLAN.md` was intentionally changed; its pre-amendment SHA-256 was
  `4124c8df3aaabc769601c2b2415d61ea0371d8bc90789c8dcb245f042f2a73e0`.
- No new code or tests; no build, runtime suite or performance measurements are
  claimed for this documentation-only amendment. Prior report results are cited
  as prior evidence, not rerun validation.
- A non-roadmap file-integrity snapshot confirmed that this amendment changed no
  runtime, test, README, config or launcher file.

**Applied documentation-only amendment. The roadmap's blocking gates still govern.**
