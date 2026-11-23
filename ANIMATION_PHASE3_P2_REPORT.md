# Phase 3-P2 — workspace crossfade input, rendering and resources (test-only)

## Inspect/plan statement (written before code)

Entry: the user asked for 3-P2 on 2026-11-23. The 3-P1 gate passed after
review (`ANIMATION_PHASE3_P1_REPORT.md`). Phase 2 strict stays BLOCKED.

Scope:

- Test-only. No change to `src/`. No production atom or setting.
- New file `test/ws-render-xvfb.c`. It holds a simulated WM, app connections
  and a separate *rendering* model process. The model keeps outgoing scenes
  with the 3-P1 rules and draws an opacity-only crossfade with XRender.
- The model uses manual redirection and the composite overlay window. The
  overlay has an empty input shape, so input goes to the real windows.
- The crossfade duration is a model argument (1000 ms in the test). It is a
  setting, not a protocol field.
- The driver uses the `view()` order: PREPARE → desktop → unmaps → maps →
  focus to the incoming client.
- Paint order: live windows → outgoing scene at alpha 1−p, in rank order,
  clipped to the monitors and the member shape → stationary windows
  (override-redirect and DOCK) on top.

Probe results before code (private Xvfb):

- An overlay with an empty input shape passes a real XTest click to the
  window below it (PASS).
- XTest works through `dlopen("libXtst.so.6")`. No libXtst development files
  are installed.
- `XRRSetMonitor` defines extra monitors. The automatic "screen" monitor
  cannot be deleted (BadValue). In the test, the model uses the test-defined
  monitors when any exist. This rule is test-only.
- `XRRSetMonitor` sends no RRScreenChangeNotify. A monitor change without a
  root resize is therefore visible only through the WM monitor epoch.

Checks:

1. Crossfade: outgoing colour early, a blend in the middle, the exact
   incoming pixel after the end.
2. A dock stays at full colour during the whole fade.
3. Input: click, wheel, key and stationary hover. Log the receiving client
   and the displayed pixel at that moment, as separate fields.
4. Stalled model (`SIGSTOP` mid-fade): input still goes to the incoming
   client. After `SIGCONT`, the exact final frame appears quickly.
5. Mid-fade cancel by monitor epoch, owner change and a second
   `_NET_WM_CM_S0` owner: exact final frame, then the XRes baseline.
6. A→B→C→A storm: at most one held scene, a correct final frame, XRes
   baseline.
7. No frames after the end or after a cancel.
8. Shared budget with a simulated close-layer reserve: bypass.
9. Asynchronous X error: another client frees a scene Picture mid-fade.
10. ARGB and shaped members; empty outgoing workspace; a restack between
    PREPARE and the unmaps; unequal monitors with a hole; a member with a
    negative x.

Labels, not mechanics: Present is SKIP (the model only copies). Fullscreen
and scratchpad are 3-I policy items. `_NET_WM_WINDOW_OPACITY` is not covered.

The input/visual result is a user decision, as for the Phase 2 strict gate.
Correct delivery alone does not pass it.

## Decision: gate PASS (reviewed 2026-11-23), option 2

All prototype checks pass on a private Xvfb (`make check-ws-render`, 3 runs
in a row after the last change). The crossfade, stationary UI, cancellation,
storm, budget, error and resource checks pass.

Input goes to the correct client in every case. That is **not** enough to
pass the gate. The outgoing image dominates the screen for about half of
the fade while input already goes to the incoming workspace. A stalled
compositor extends this without limit. This is a user decision, like the
Phase 2 strict gate (see "Input/visual ambiguity").

The user reviewed this report and chose option 2: accept the mismatch, but
cap `animation_workspace_ms` at 0..250 ms, default 0 (opt-in). The mismatch
is then at most about 125 ms (computed). 3-I must document it in the README.

No `src/` change. No production protocol is frozen. 3-I needs a separate
request. Phase 2 strict stays BLOCKED.

## Files (test-only)

- `test/ws-render-xvfb.c`: simulated WM and app connections, XTest input,
  and the rendering model in a separate process (`model` mode).
- `test/test-ws-render.sh`, `make check-ws-render`: private Xvfb
  1600×700 only.
- `test/ws-capture-prototype.h`: new `wsp_shared_fits()` for the shared
  budget. The 3-P1 API and behaviour are unchanged. `check-ws-capture` and
  the headless checks still pass.

Test layout: monitor A 0,0 800×600 and monitor B 900,0 640×480 (gap at
x 800..900). Crossfade duration 1000 ms.

## Results

| # | Check | Result |
| --- | --- | --- |
| 1 | Early frame (≈ 40 ms): mostly the outgoing image | PASS (`c50007`: red over blue) |
| 1 | Middle frame (≈ 490 ms): a blend | PASS (`680064`) |
| 1 | Exact final frame after DONE (incoming, monitor B, gap) | PASS |
| 2 | Dock (override-redirect) at full colour early and in the middle | PASS (`ff00ff`) |
| 10 | Gap between monitors shows no scene pixel | PASS (`202020`) |
| 10 | Shaped member: hole shows the incoming client exactly; body fades | PASS (`00cc00`, `f6fdf6`) |
| 10 | ARGB member (50 % red) blends with its own alpha | PASS (`8c1111`) |
| 10 | Member with negative x: visible part drawn | PASS (`c6c601`) |
| 3 | Click, wheel (button 4) and key at 42 ms go to the incoming client | PASS (incoming 2 buttons, 1 key; outgoing 0) |
| 3 | Stationary pointer: incoming client gets EnterNotify at the switch | PASS (1) |
| 7 | No frames in 500 ms after DONE | PASS (0; 105 frames during the fade) |
| 4 | `SIGSTOP` mid-fade: frame frozen for 1200 ms | PASS (`1e00ae` both samples) |
| 4 | Click during the stall goes to the incoming client | PASS |
| 4 | Exact final frame after `SIGCONT` | PASS (5–10 ms) |
| 5 | Monitor change (`XRRSetMonitor`) + epoch mid-fade: END epoch, exact final frame | PASS |
| 5 | WM owner change mid-fade: END owner, exact final frame | PASS |
| 7 | No frames in 300 ms after a cancel | PASS |
| 10 | Restack between PREPARE and the unmaps: raised member on top | PASS (`ce0500`) |
| 6 | A→B→C→A storm, 6 switches 30 ms apart | PASS (5 superseded, last DONE, exact final frame) |
| 6 | Peak scene bytes after the storm stay within the budget | PASS (1412000 of 33554432) |
| 8 | Shared budget: close-layer reserve leaves no room | PASS (BYPASS budget, final frame at once) |
| — | Outgoing member and incoming client destroyed mid-fade | PASS (kept image still drawn `6e08c7`; DONE; exact final frame shows the background) |
| 9 | Another client frees a scene Picture mid-fade (async error) | PASS (END xerror, exact final frame) |
| 10 | Empty outgoing workspace | PASS (ADMIT 0, DONE, 1 frame) |
| — | Model XRes after all of the above | PASS (33/33 resources, 5485328/5485328 bytes) |
| 5 | Second owner of `_NET_WM_CM_S0` mid-fade (SelectionClear) | PASS (END replaced; server then shows the final workspace) |
| — | Unexpected X errors in the model | PASS (0) |

XRes note: `CompositeClientWindow` records go from 13 to 14. Composite keeps
one record for each child of root while the model redirects. The driver
creates one new WM owner window in check 5. This count is reported apart
and is not a leak.

## Input/visual ambiguity (user decision)

- From the switch until about **500 ms** (half of a 1000 ms fade), the
  outgoing image dominates the probe pixel. Click, wheel, key and hover
  already go to the incoming client during that time.
- At a 180 ms setting (`animation_workspace_ms` in the plan), this is
  about 90 ms. This number is computed from the linear alpha. It was not
  measured.
- The plan's duration range is 0..1000 ms. At 1000 ms the old image
  dominates for about 500 ms (measured).
- If the compositor stalls, the last frame stays on screen for the whole
  stall (1200 ms in the test). Input still goes to the incoming client. Any
  compositor has this behaviour, but a crossfade makes the stale frame show
  the *old* workspace.
- No input shield, grab, replay or delayed focus is used. The plan forbids
  them.

Options for the review:

1. Accept as opt-in best-effort, documented like Phase 2. Keep the
   0..1000 ms range. A user who sets 1000 ms sees the old workspace for
   about 500 ms while input goes to the new one.
2. Accept, but cap `animation_workspace_ms` lower (for example 0..250 ms).
   The mismatch is then bounded at about 125 ms (computed).
3. Reject. Workspace switches stay instant.

## Findings

1. **An input-transparent overlay works.** An empty input shape on the
   composite overlay sends real XTest clicks to the window below.
2. **Clipping per monitor works with XFixes regions.** The gap and the
   shape hole show no scene pixel. A member that crosses the gap is drawn
   only on the monitors.
3. **The dock must be drawn after the scene.** It is not a member, and it
   stays at full colour.
4. **Ranks must be read again when the scene is complete.** A restack after
   PREPARE is then drawn correctly.
5. **A monitor change without a root resize sends no RandR event on Xvfb.**
   The WM epoch is the only reliable signal in this test.
6. **Sampling by monotonic time recovers from a stall at once.** After
   `SIGCONT`, the next frame is the exact final frame.
7. **Freeing a Picture from another client gives an async error.** The
   model must drop that Picture without freeing it again.

## SKIP and open items

- Present: SKIP. The model only copies to the overlay. dani-comp's Present
  path belongs to 3-I.
- Hardware pacing, tearing and real input latency: SKIP (Xvfb).
- Per-monitor topology from real outputs: only test monitors (the automatic
  Xvfb monitor cannot be deleted).
- `_NET_WM_WINDOW_OPACITY` multiply: not covered.
- Fullscreen: 3-I policy. The WM sends no PREPARE when the outgoing or
  incoming workspace has a fullscreen client.
- Sticky scratchpad: 3-I policy. It is not a member. It is drawn live, above
  the scene, like stationary UI.
- The shared budget uses a test property for the close-layer reserve. In
  dani-comp it must come from the exit pool itself.
- daniwm publishes no monitor epoch today. 3-I must add one and bump it on
  RandR and Xinerama changes, because a monitor-only change can arrive with
  no RandR event (Finding 5).

## Next step

3-I (production integration) needs a separate request. Carry the 0..250 ms
cap, the open items above and the fullscreen/scratchpad policy into its plan.
