# daniwm animation roadmap

Status (branch `feat/animation`, not released):

| Phase | Status | Evidence |
| --- | --- | --- |
| 0 | Gate PASS. Test-only prototype. | `ANIMATION_PHASE0_REPORT.md` |
| 1A | Gate PASS. Opt-in, off by default. | `ANIMATION_PHASE1A_REPORT.md` |
| 1B | Gate PASS. Opt-in, off by default. | `ANIMATION_PHASE1B_REPORT.md` |
| 2 strict | BLOCKED. The strict input gate is not met. | `ANIMATION_PHASE2_PROTOTYPE_REPORT.md`, `ANIMATION_PHASE2_ELIGIBILITY_REPORT.md` |
| 2 best-effort | Implemented. Opt-in, `animation_layout_ms = 0` by default. Strict gate still unmet. | `ANIMATION_PHASE2_BESTEFFORT_REPORT.md` |
| 3-P1 | Gate PASS (reviewed 2026-11-23). Test-only. No runtime code. | `ANIMATION_PHASE3_P1_REPORT.md` |
| 3-P2, 3-I | Not started. Each needs a separate request. | `ANIMATION_PHASE3_PREREQUISITES_REPORT.md` |
| 4 | Not started. | — |

Each branch is reviewed on its own and must pass its own exit gate. A Phase 3
prototype does not mark Phase 2 complete.

## 1. Goals and boundaries

Add restrained, optional animation without changing daniwm's tiling, focus,
workspace, input, or EWMH semantics. Start with fades; add layout transitions;
then workspace and overview effects. Preserve the instant, non-composited path.

Recommended defaults:

- WM-managed animation stays off until explicitly enabled.
- Reduced motion disables every effect; zero duration means instant completion.
- No animation delays focus, input delivery, client removal, or workspace state.
- No new rendering dependency initially: use the existing XRender compositor.
- Direct mouse dragging/resizing, fullscreen transitions, docks, menus, tray and
  unmanaged override-redirect windows remain instant in the first releases.
- No finger-follow gesture animation, new gesture grabs, spring physics, blur,
  or live previews of hidden applications in this roadmap.

## 2. Current implementation and constraints

| Area | Current behavior | Planning consequence |
| --- | --- | --- |
| `src/comp.c` | XComposite/XDamage/XRender compositor with optional XPresent and copy fallback. `win_alpha()` implements map/remap fade-in; default duration is 160 ms. | Extend the existing render loop rather than add another renderer or timer thread. |
| Compositor scheduling | Monotonic clock, `dirty`/`fading`, `select()`, Present completion/idle deadlines; `FRAME_INTERVAL_MS` is 8 ms despite an older 60 Hz comment. | Reuse scheduling; verify actual pacing rather than promise a fixed refresh rate. |
| Compositor lifecycle | Unmap frees the window pixmap; destroy deletes the `Win`. Resize/border changes also invalidate pixmaps. | Fade-out and resize transitions require explicitly owned snapshots with bounded lifetime. |
| `src/keys.c` | `k_compositor()` launches dani-comp with `--shadow --no-fade --dim 1`. | Existing compositor fade support is not enabled by the normal WM launcher. |
| `config`, `autostart.sh` | Example configuration documents a separate compositor launch path. | Cover both WM-managed and manually launched compositors; avoid duplicate processes. |
| `src/layout.c` | Tile/monocle/fullscreen paths issue immediate `XMoveResizeWindow`; arrange work is coalesced on some paths. | Compute target rectangles once; do not call arrange or reconfigure clients every animation frame. |
| `src/client.c` | `view()` captures overview previews, unmaps the old workspace, arranges the new one and updates focus. | Preserve this semantic ordering; outgoing visuals must not keep old clients mapped. Audit `send_to()` and external moves too. |
| `src/overview.c` | WM-owned overlay with keyboard grab and cached previews; generic override-redirect windows are excluded from current fades. | Overview needs an explicit opt-in visual role, not a global override-redirect exception. |
| Overview cache | Up to 64 previews, each at most 192×108; only viewable clients are captured with compositor ownership present. | Keep these limits and behavior. This low-resolution cache is not a full-screen animation buffer. |
| `src/swipe.c`, `src/gestures.c` | Completed gestures trigger workspace/overview actions. | Reuse those actions unchanged. Animation does not prove or fix physical gesture delivery. |

## 3. Architecture recommendation

### Ownership

The WM owns **logical state**: target geometry, mapping, stacking, focus,
workspace selection and overview interaction. The compositor owns **visual
interpolation**: opacity, displayed rectangles and temporary image layers.

Prefer compositor-side layout interpolation over per-frame real client resizing.
Applications receive their final geometry once, avoiding resize storms and
repeated terminal/browser reflow. Initially scale a retained image during resize
and replace it with final live content when ready; assess temporary text softness
and completion flashes in the prototype.

This choice has a known input tradeoff: X input targets the real final window,
not its animated picture. Keep transitions short and cancel/snap affected visuals
before pointer interaction. Do not add invisible input shields or grabs. Prototype
click/hover behavior before approving this architecture for broad use. The Phase 2
movement prototype did not establish a safe admission policy, so the strict
layout-movement gate stays BLOCKED. The opt-in best-effort path accepts the known
click/hover mismatch. The default layout path stays instant.

Opacity-only workspace crossfades are a separately gated rendering investigation:
they do not interpolate client rectangles, but still require workspace-specific
capture, input/visual, monitor and resource prototypes before integration. This
technical separability is the approved dependency exception, not evidence that
workspace animation is safe or implemented.

### Shared animation model

Introduce a small deterministic animation module (proposed `src/animation.c/.h`):
start time, duration, start/end opacity or rectangle, easing, current sample,
completion and cancellation. Inject time in tests. Begin with linear fades and
cubic ease-out movement, not configurable arbitrary easing expressions.

- Sample from monotonic elapsed time, never accumulated frame counts.
- Retarget from the currently displayed value; newest target wins.
- Key state by client identity plus generation, not a reusable XID alone.
- Snap to the exact final value, release images, then stop animation wakeups.
- Support independent per-window and per-workspace cancellation domains.
- Do not permanently overwrite application `_NET_WM_WINDOW_OPACITY`; multiply
  application opacity, existing dim behavior and animation alpha at paint time.

### WM/compositor coordination

Fades can start locally, but later phases need explicit transition reasons so a
workspace unmap is not treated as an application closing. For Phase 2, specify a
small versioned private X11 protocol, with a capability/owner check, sequence IDs,
bounded validated payloads, cancellation and stale-message rejection. Working atom
names such as `_DANIWM_ANIMATION_*` remain proposals, not implemented API; the
existing hide/settings channels and test-only layout atoms are not an approved
workspace protocol.

Do not assume a notification sent before resize/unmap means the compositor has
already captured the old image: these processes use separate X connections.
Phase 2's strict movement gate stays BLOCKED because its prototype did not prove
safe input/capture ordering. The best-effort path does not pass this gate. Any
future strict Phase 2 design must pass that original gate on its own.

A separately requested Phase 3 workspace branch may prototype an asynchronous
prepare/ready/commit hypothesis, with capability, owner/monitor epochs, workspace
and member generations, bounded payloads, cancellation, expiry and immediate
bypass. Readiness must never delay focus, input, unmap, EWMH publication, arrange
completion or any other WM transition. Late or missing readiness skips the effect;
it never revives a stale switch. Freeze no production protocol until the capture
and adversarial-ordering prototype passes review, followed by independent
input/visual, monitor and resource/rendering review. Never block the WM waiting for
an acknowledgment or hold an X server grab.

On compositor loss, replacement, timeout or unsupported protocol, continue with
normal instant WM behavior. External compositors are not required to implement
private effects, and the WM must not steal their redirection/selection ownership.

## 4. Phased delivery

### Phase 0 — Baselines and lifecycle prototype

- Record current fades, map/unmap/destroy, workspace switch, layout resize,
  overview show/hide, opacity and compositor replacement behavior.
- Measure idle wakeups, paint count, frame intervals and X resource use on a
  documented isolated display; inspect existing damage and Present tests.
- Prototype compositor-owned snapshot lifetime across unmap/destroy/resize,
  including ARGB, borders, shaped windows and asynchronous X errors.
- Set snapshot count, byte and timeout limits before retaining full-size images;
  budget by actual dimensions/depth, not just number of windows. On overflow,
  cancel an older effect or skip capture. Never save application images to disk.

**Exit gate:** snapshot ownership/cleanup is demonstrated, architecture and
input tradeoff are reviewed, and existing regressions remain green. No feature
is installed or enabled in the live session.

### Phase 1A — Configurable fade-in and reduced motion

- Reuse existing map fade; add the shared deterministic sampler.
- Add validated settings and a deliberate policy for reload and manual launches.
  Preserve current standalone CLI defaults unless separately approved.
- Pass settings when the WM launches dani-comp. For compatible running dani-comp,
  use a narrowly scoped settings channel rather than killing it on every reload;
  standalone CLI settings remain authoritative unless WM control was opted into.
- Disabling motion cancels current effects immediately and repaints final state.
- Cover zero duration, invalid input, application opacity, dim and shaped windows.

**Exit gate:** enabled fade-in works, default WM startup remains instant, and
reduced motion leaves no animation-driven repainting after cleanup.

### Phase 1B — Fade-out with explicit lifecycle reasons

- Detach a compositor-owned image from live client bookkeeping; do not continue
  querying a destroyed client. Render the temporary image in a bounded visual
  layer, with explicit stacking, alpha and cleanup.
- Distinguish close/withdraw from workspace hiding, monocle hiding and overview
  teardown. Workspace and monocle transitions remain instant at this phase.
- Closing can happen without advance WM notice: retain usable content ahead of
  time where safe, otherwise skip the exit effect. Never delay client destruction.
- Handle destroy-before-first-damage, rapid remap and XID reuse without ghosts.

**Exit gate:** repeated open/close/remap bursts do not leak resources, leave
stale images, change focus or accidentally fade every workspace switch.

### Phase 2 — Layout and discrete window movement — best-effort implemented

Status: implemented on `feat/animation`, opt-in, best-effort. Not released. The
strict gate is unmet. See `ANIMATION_PHASE2_BESTEFFORT_REPORT.md`.

Phase 2 is implemented as an explicitly opt-in **best-effort** branch under the
revised dependency approved for this repository. It does **not** satisfy the
original strict input/visual-consistency gate: the retained picture may lag the
final X hit-test geometry, and this known mismatch is documented and accepted.
The instant path remains the default (`animation_layout_ms = 0`). Direct mouse
manipulation, fullscreen, monitor changes, reload and unsupported content remain
instant/cancelled. This is not a claim that the earlier strict blocker was solved.

The implementation retains the final-geometry ownership rule and does not use
input grabs, replay, shields, delayed focus/input or per-frame real-client resize.
It is bounded, fail-closed and opt-in; missing/stale/unsafe frames skip visual
movement and leave the logical transition immediate.

- Refactor layout calculations to expose old/target rectangles while preserving
  current geometry rules and arrange coalescing.
- Send one transition transaction per arrangement. Commit real target geometry
  once and animate only presentation; pure moves may use live content.
- First support tile rearrange, zoom/swap, discrete gap/master changes and
  tile↔floating transitions where the prototype proves input behavior acceptable.
- Retarget rapid commands from their current visual state; cancel for direct
  mouse manipulation, client removal, fullscreen, monitor changes and reload.
- The shipped best-effort path supports fixed-size opaque, unshaped depth-24
  tiled movement pictures. Resizes, ARGB/translucent/shaped content and other
  unsupported cases fall back to the final live client instantly.
- Damage must cover old/new displayed extents plus shadows. Start conservatively
  with full repaint for effects; optimize only after correctness is verified.
- Exclude transformed/translucent images from opaque occlusion shortcuts; reset
  XRender transforms, filters and clipping before reusing pictures.

**Best-effort release gate:** final geometry matches the instant path; clients
receive no per-frame resize storm; the documented click/hover mismatch is
accepted rather than hidden; bounded retained resources settle to the newest
layout without stale layers or leftovers. The original strict guarantee
(`clicks/hover do not target stale positions`) remains an unmet limitation and
must not be represented as passed.

### Phase 3 — Workspace transitions

- Start with a short crossfade; consider directional slide only after it passes.
- Capture outgoing visuals while viewable using the approved coordination path;
  switch workspace/focus/EWMH once as today. Never map hidden apps for capture.
- Use bounded compositor-owned workspace layers, not overview thumbnails.
- Keep docks/bar/tray stationary. Clip each monitor separately, accounting for
  negative coordinates, unequal sizes, fullscreen and empty workspaces.
- Coalesce rapid switches: newest logical workspace wins; flatten or discard
  old layers within budget, never build an unbounded transition queue.
- On capture failure, compositor loss, monitor change, reload or incompatible
  owner, remove temporary layers and show the final workspace instantly.

**Dependency and subgates:** Phase 3 is a separately gated workspace branch, not
an implementation continuation of blocked Phase 2. A future request must first
pass **3-P1**, a test-only capture/protocol prototype: demonstrate fresh,
complete, correctly owned outgoing scenes or a fail-closed bypass under adverse
ordering, while proving WM state transitions never wait. After review, a separate
**3-P2** test-only input/render/resource prototype must validate incoming input and
focus semantics, stationary UI, per-monitor composition, cancellation, aggregate
budgets and cleanup under delayed scheduling and switch storms. Only an explicit
later request after both reviews may enter **3-I** production integration. A
negative or unavailable result keeps the workspace path instant and blocked; it
does not weaken the Phase 2 gate or authorize hidden mapping, shields, grabs or
live previews.

**Exit gate:** workspace-storm, focus, hidden-client, fullscreen and scratchpad
regressions pass; no input remains assigned to an outgoing workspace and no
outgoing image survives cancellation.

### Phase 4 — Overview polish

- Add explicit overview-role opt-in for opacity first; consider a subtle scale
  only after preview/text quality and hit-testing have been evaluated.
- Keep opening, browsing, commit and cancel semantics unchanged. Release the
  keyboard grab immediately on hide; an exit animation is visual only.
- If an exit image remains, make its presentation input-transparent without
  changing generic menu behavior. Rapid show/hide must not stack grabs/layers.
- Animate pending-selection highlighting without committing the workspace.
- Preserve preview bounds, labeled fallback cards, pointer-monitor placement,
  compositor-free operation and existing no-hidden-mapping behavior.

**Exit gate:** overview controller/cache/render checks pass; Escape restores the
same workspace/focus, selection remains pending until commit, and grabs/resources
are released on every failure and teardown path.

## 5. Configuration

Supported now (`src/config.c`):

```ini
compositor = 0
animations = 0
reduce_motion = 0
animation_open_ms = 140
animation_close_ms = 100
animation_layout_ms = 0     # best-effort; 0 = instant
```

Proposed only (not supported yet):

```ini
animation_workspace_ms = 180
animation_overview_ms = 140
```

Add each setting only with its corresponding phase. Proposed duration range:
0–1000 ms; reject invalid/overflowing values with diagnostics. `animations = 0`
or `reduce_motion = 1` overrides all durations. Workspace crossfade is the initial
fixed effect; do not advertise slide until implemented and verified. Keep
`compositor = 1` separate: enabling animation does not silently start a compositor.

## 6. Verification and release gates

**Pure unit tests:** interpolation/endpoints, easing bounds, long frame gaps,
zero duration, cancellation, retargeting, generation checks, settings validation,
protocol decoding and resource-budget accounting. Proposed `make check-animation`
should run without a display.

**Private X server integration:** extend `test/test-comp.sh`,
`test/test-comp-present.sh`, layout/workspace tests and overview Xvfb coverage.
Exercise Present-enabled and fallback builds, opacity/ARGB/shape, resize/unmap/
destroy races, compositor replacement, timeout, reload/restart, monitor changes,
resource-budget exhaustion and malformed/stale protocol messages. Retain
`make check-swipe` and `make check-overview` as independent regression gates.

**Visual/input validation:** on an explicitly approved isolated Xephyr/Xorg setup,
check tearing, final-frame flashes, click/hover during movement, typing, dragging,
menus/transients, fullscreen, mixed monitors and rapid action bursts. Xvfb proves
neither hardware pacing nor physical touchpad delivery; record unavailable checks
as SKIP/BLOCKED, not PASS. Do not run probes on the user's active display.

**Performance acceptance:** report hardware, resolution, backend and baseline.
Require no animation timer after completion (existing compositor maintenance may
still wake), bounded snapshot memory, no monotonic X-resource growth over repeated
cycles, no client resize requests per visual frame, and unchanged idle behavior
when disabled. Record frame-time distribution and input latency; establish numeric
targets from Phase 0 rather than inventing hardware-independent FPS guarantees.

**Release order:** small commits/PRs for baseline tests, sampler, fade settings,
exit snapshots, and the separately reviewed protocol/layout/workspace/overview
branches. Phase 2 strict input-safe effects remain unmet; its opt-in best-effort
implementation is a separate commit on `feat/animation` and is not released. Phase 3 may be requested through 3-P1
capture/protocol, 3-P2 input/render/resource, then 3-I integration;
none is implied by this roadmap amendment. Update `README.md`, example `config`
and launcher documentation only as each corresponding feature ships. Keep
animations opt-in until visual/input verification is complete. Installation,
restart and live-session testing require separate user approval.
