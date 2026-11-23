# Phase 1A — configurable opening fade and reduced motion

Status: **PASS — implemented and privately verified; not installed or enabled
in the user's running session.** Phase 0 evidence is in
`ANIMATION_PHASE0_REPORT.md`. No Phase 1B/2/3/4 features were implemented.

## Delivered behavior

```ini
compositor = 0
animations = 0
reduce_motion = 0
animation_open_ms = 140
```

- All booleans accept existing config boolean spellings; invalid values produce
  diagnostics and keep the previous/default value. Duration must be an integer
  in 0..1000, with overflow/trailing-data rejection.
- The old README described `compositor` but the parser no longer implemented it.
  It is now an explicit startup opt-in; animations alone never launch anything.
  Startup skips an existing compositor selection owner, including foreign ones.
- WM-launched dani-comp uses `--shadow --no-fade --dim 1 --wm-settings`.
  Valid WM configuration controls fade, with compositor=0 / animations=0 /
  reduce_motion=1 / duration=0 all taking the instant path.
- Opening/remap fade only; docks, fullscreen and override-redirect windows are
  instant. No close, movement, layout, workspace or overview effects exist.
- `reload_config` publishes settings to compatible opted-in dani-comp without
  killing/restarting it or waiting for an acknowledgment. Actual motion-setting
  changes snap existing fades to their final alpha; enabling affects subsequent
  maps only. Unchanged settings do not restart current fades.
- Reload changes motion policy, not process ownership: toggling `compositor` in
  config does not stop/start a process. Use explicit compositor action or next
  startup. The action may start an instant compositor with compositor=0.
- The example autostart uses fastcompmgr. Documentation warns to disable that
  launcher before choosing WM-managed dani-comp. No live config was modified.

## Standalone and private settings contract

Standalone CLI defaults remain fade-in 160 ms, dim 0.92, shadow enabled. Normal
CLI instances ignore WM animation settings. `--wm-settings` explicitly replaces
fade policy with valid WM settings; absent/malformed/stale settings or WM loss
means instant mode, not fallback to potentially enabled CLI fade. CLI
`--reduce-motion` is always a hard override, including under WM control.
`--fade-ms`/`--fade-time` now validate the integer 0..1000 range rather than
silently accepting malformed input or clamping out-of-range values.

The settings channel is a root property `_DANIWM_ANIMATION_SETTINGS_V1` containing
exactly five CARDINAL/32 words: version=1, supporting WM window, enabled,
reduce-motion, opening duration. The WM checks its compositor scalar when
publishing enabled. Readers validate type/format, exact length (including no
extra payload), version, booleans, range and a live, self-referencing supporting
WM window. Destroying that window or changing root WM/settings properties
revalidates and cancels existing motion when necessary. No properties are written
to application windows; no application opacity is replaced.

This is a narrow settings channel, not capture/geometry coordination or a security
boundary against other clients on the same trusted X server. There is no IPC
socket, polling daemon, timer, thread, capture handshake or per-frame WM work.

The pre-existing compositor toggle used `pkill -x dani-comp`, which could kill
compositors on unrelated displays. To keep the supported launch path safe,
compatible dani-comp now exposes a versioned control marker on its selection
window and accepts a scoped stop ClientMessage. The WM sends it only to a
compatible current owner on its display; foreign/older owners are left alone.
No PID reuse/signaling ambiguity or global process-name kill remains there.
Older compositors without this marker cannot be stopped by this action.

## Implementation boundaries

- `src/animation.c/.h`: deterministic linear sampler. Callers supply monotonic
  milliseconds; no frame-count accumulation. State is embedded per tracked
  window and keyed by `(window identity, generation)`. Generations increase on
  tracking creation; overflow fails instant instead of reusing an identity.
- `src/comp.c`: starts/samples/cancels embedded fade state. `born` remains
  independent for existing shape rechecks. Unmap cancels and frees cached live
  pictures as before; destroy removes all embedded state with the tracked Win.
  There is no detached image, new snapshot or extra Picture/pixmap retention.
- Application opacity and per-pixel ARGB alpha are multiplied by fade alpha;
  dim remains the existing separate black overlay. Fades/ARGB/shapes are excluded
  from opaque occlusion shortcuts. Fade frames repaint their full extents even
  when a concurrent content-damage box is small, including the final frame.
- No XRender transform/filter reuse is added: this phase only adjusts opacity.
- WM geometry, tiling, focus, mapping, workspace, input and EWMH handlers are not
  changed. No real client resize occurs per animation frame. The WM publishes
  settings only at startup and reload; compositor rendering stays in its existing
  select/Present/copy loop. Instant states do not schedule animation frames.
  Existing shape checks and 5-second maintenance GC are unchanged.

## Tests and verification

### New/extended coverage

- `make check-animation`: sampler endpoints/long gaps/zero duration/cancellation,
  identity-generation mismatch, versioned-payload validation, scalar defaults,
  valid booleans and invalid/overflowing durations. Includes previous Phase 0
  headless ownership tests unchanged.
- `test/comp-fade.c` via `make check-animation-xvfb`: real pixel fade/cancellation,
  unchanged opacity property, malformed/deleted/stale-owner settings, standalone
  isolation, CLI reduced-motion override, enabling without retroactive fade,
  shaped-hole clipping, ARGB pixel alpha and tracking-generation replacement.
- `test/comp-animation-loop.c`: five modes (fade, instant, zero, reduced, mid-fade
  cancellation), each on copy and actual Xvfb Present. All ten runs have zero
  paints/events/timers in their 1.5-second settled window; measurement deadline
  wakeups are counted separately. The cancellation fixture turns reduced motion
  on during a 1000 ms fade and stops it without continuing the original duration.
- `test/animation-settings-xvfb.c` via `make check-animation-wm`: real WM config
  parsing/publication, automatic compositor startup, visible fade, reduced-motion
  reload with same compositor owner and retained focus, animations=0, duration=0,
  compositor=0, scoped stop/start and leaving a mock foreign owner untouched.
  Previous no-compositor and manual-compositor semantic baselines still run.
- Existing compositor Present-buffer tests link the new sampler; their storage,
  serial, completion/idle ordering, timeout and fallback assertions remain intact.

### Results

| Check | Result |
| --- | --- |
| Full `make -B -j2 all` | PASS, no compiler warnings. |
| `make check-animation` | PASS. |
| `make check-animation-xvfb` | PASS copy/Present builds, renderer and ten loop modes; Phase 0 snapshot experiments remain test-only. |
| `make check-animation-wm` | PASS baseline and settings/launch/reload/scoped-toggle modes. |
| ASan + UBSan for all three animation targets | PASS, no suppressions; production children in WM tests are normal builds. |
| `make check-swipe check-overview` | PASS offline and private-Xvfb coverage. |
| Existing X11 regression suites | **42/42 PASS** in UID-preserving Bubblewrap with private namespaces, devices, temporary files, session environment and Xvfb displays. |
| Shell syntax / `git diff --check` | PASS. |

Session logs:

- Full rebuild: `/var/tmp/daniwm-phase1a-build.GzkZtD`.
- Full regressions: `/var/tmp/daniwm-phase1a-regressions.k4vN9w`.
- Final normal feature checks: `/var/tmp/daniwm-phase1a-final-tests.vg0b0j`.
- Final ASan/UBSan: `/var/tmp/daniwm-phase1a-sanitizers-final.RimfKG`.

The 42-suite pass covers existing config, mouse, workspace, focus/input, fullscreen,
scratchpad, restart, tray, compositor, Present and launcher behaviors. Previously
reported AI/Python import compatibility failures are unrelated and were not
modified or treated as an animation acceptance gate.

## Exit gate and remaining scope

Phase 1A gate is satisfied: enabled fade works, default WM startup remains
instant, standalone policy is preserved, reduced motion/zero/disabled settings
snap to final opacity, and all ten measured paths settle without animation-driven
repainting. No production snapshot resources or new animation timer are added.

Xvfb pixel and semantic assertions do not establish hardware tearing, physical
input/gesture delivery or subjective appearance on the user's display. No live
or hardware-backed session test, installation, binary deployment, restart or user
config change was performed. Those require separate approval. Phase 1B must be
requested and reviewed independently before any closing snapshot/layer is added.
