# Phase 3-P1 — workspace capture and protocol prototype (test-only)

## Inspect/plan statement (written before code)

Entry: the user asked for 3-P1 ("do all 4"). Phase 0, 1A and 1B gates pass on
`feat/animation`. Phase 2 strict stays BLOCKED. This prototype does not change
that.

Scope:

- Test-only. No change to `src/`, no production atom, no setting, no launcher
  flag. The atoms are `_DANIWM_TEST_WS_*` and live in a test header.
- The WM is simulated on its own X connection, as in the Phase 2 prototype.
  The compositor model is a separate process with its own connection.
- No rendering and no input checks. These belong to 3-P2.

Candidate strategy (hypothesis 1 of the prerequisites report):

1. The compositor model names a pixmap for each window when it sees the map
   (automatic redirection). It drops and renames the pixmap on resize.
2. Before the unmaps, the WM sets a transaction property and sends an ordered
   PREPARE ClientMessage with the same sequence. It never waits for a reply.
3. At PREPARE, the model checks capability, owner, epoch, sequence, age and
   budget. At each member UnmapNotify, it keeps the named pixmap.
4. If any member has no usable pixmap, the whole scene bypasses. A partial
   scene is never admitted.
5. At `_NET_CURRENT_DESKTOP` = target, a complete and fresh scene is admitted.
   A newer PREPARE, owner change, epoch change, TTL or late message frees it.

Pass criteria: every adverse case either admits a correct scene or bypasses
with the expected reason. The WM sequence completes with the model stopped.
XRes count and bytes return to baseline. No unexpected X error occurs.

## Decision: prototype checks pass; gate decision pending review

All prototype checks pass on a private Xvfb. Every adverse case either
admitted a fresh and complete scene or bypassed with a reason. The simulated
WM never waited for the model. Only a reviewed prototype can establish the
coordination path, so the 3-P1 gate is not passed yet. This is not approval
for 3-P2 or 3-I. No production protocol is frozen. Phase 2 strict stays
BLOCKED.

## Files (test-only)

- `test/ws-capture-prototype.h`: test atoms, bounded codec, sequence and age
  checks, padded image bytes, generation-bound scene state.
- `test/test-ws-capture-protocol.c`: headless checks. They run in
  `make check-animation`.
- `test/ws-capture-xvfb.c`: simulated WM and app connections, plus the
  compositor model in a separate process (`model` mode).
- `test/test-ws-capture.sh`, `make check-ws-capture`: private `Xvfb -displayfd`
  only.

No file in `src/` changed for 3-P1.

## Experimental transaction

Root property `_DANIWM_TEST_WS_TXN_V1` (format 32), at most 26 words:
version, WM owner, model owner, monitor epoch, sequence, from, to, sent time
(CLOCK_MONOTONIC ms, low 32 bits), deadline (1..1000 ms), count (0..16), then
the member XIDs.

The WM order on its own connection. This is the order of `view()` in
`src/client.c`: `ewmh_desktops()` runs before the unmap loop.

1. Read the model capability (an X server round trip, not a model reply).
2. Set the transaction property.
3. Send a PREPARE ClientMessage with the same sequence to root.
4. Set `_NET_CURRENT_DESKTOP` (the commit).
5. Unmap the outgoing members. Map the incoming members.

Because the commit comes before the unmaps, the model records it and admits
the scene when the last member is kept. Two labeled cases use a
"commit-last" order (commit after the maps) to hold a complete scene.

The model rejects the PREPARE if any of these is true:

- The owner, model owner or epoch does not match.
- The sequence is not newer than the last one.
- The property already holds a newer sequence (`overwritten`).
- The message is older than its deadline (`late`).
- A member has no named pixmap for its current generation (`unnamed`).
- The sum of member image bytes is over 32 MiB (`budget`).

The model checks the budget before it keeps any image.

## Results (`make check-ws-capture`, 3 runs in a row, all PASS)

| Case | Expected | Result |
| --- | --- | --- |
| No capability | WM sends no transaction | PASS |
| Positive control (deliberately ordered), daniwm order: app paints, `XSync`, idle model | ADMIT with the new colour | PASS (`ADMIT 2 ffff00 00cc00`) |
| Commit-last order | ADMIT at commit | PASS |
| Model stopped: window maps and leaves before it is named | BYPASS unnamed | PASS |
| Model stopped: resize, then switch | BYPASS unnamed (stale size never admitted) | PASS |
| Model stopped: two switches before the first is read | first BYPASS overwritten | PASS (follow-up: `BYPASS unnamed`) |
| Model stopped 300 ms, deadline 100 ms | WM completes; BYPASS late | PASS |
| Ordered, daniwm order: owner change while the scene collects | BYPASS owner | PASS |
| Ordered, daniwm order: monitor epoch change while the scene collects | BYPASS epoch | PASS |
| Race: owner change before PREPARE is read | never admitted | PASS (`stale`) |
| Race: epoch change before PREPARE is read | never admitted | PASS (`invalid`) |
| No commit | BYPASS expired after 1000 ms TTL | PASS |
| Commit-last order: member destroyed during retention | kept image valid, ADMIT | PASS (`ADMIT 3 cc0000 00ffff cc00cc`) |
| 4096×2048 member | BYPASS budget before retention | PASS |
| Rapid A→B→A, model running (smoke only, outcome varies) | each switch reported once | PASS (seen: `superseded`/`ADMIT`, then `ADMIT`/`unnamed`) |
| 64 ordinary switches, daniwm order | 64 ADMIT; XRes back to baseline | PASS (13/13 resources, 49600/49600 bytes) |
| Commit-last order: 16 epoch cancels of a held complete scene + 2 TTL expiries | 18 ended; XRes back to baseline | PASS (13/13, 49600/49600) |
| Model X errors | 0 unexpected | PASS |

No-wait metric with the model stopped (`SIGSTOP`): enqueue 107–564 µs, server
completion 209–765 µs over 7 runs. The WM sequence does not depend on the model.

The first full run used commit-last as the default order. Review found that
daniwm commits first. With commit-first, the first model ended every scene
as `incomplete`. The model now records an early commit. The results above
are from 3 runs in a row after that change.

## Findings

1. **Freshness comes from the storage, not from a copy.** The named pixmap is
   the window storage. At unmap it holds the last content that the server has
   for that window. No capture request and no copy is necessary.
2. **The model must name each pixmap while the window is still mapped.** If
   the model is behind, naming fails after the unmap (BadMatch), and the scene
   bypasses. dani-comp already names a pixmap for each mapped window, so this
   fits the current design.
3. **A resize makes the old name stale.** The model must drop and rename the
   pixmap on each size change. If it cannot, the scene bypasses.
4. **Model reads see the current server state.** Owner and epoch changes that
   land before the model reads PREPARE give `stale` or `invalid` instead of
   `owner` or `epoch`. This is still fail-closed.
5. **The commit can arrive before the members.** daniwm publishes
   `_NET_CURRENT_DESKTOP` before it unmaps the old workspace. A compositor
   must accept an early commit and admit when the scene is complete. It must
   not treat the commit as the end of collection.
6. **Retention extends the storage lifetime.** It does not add a copy, but it
   keeps memory that the server would free at unmap. The budget counts it.

## Limits and open items

- The sent time uses CLOCK_MONOTONIC, so the WM and the compositor must run on
  one host. A production design must keep this assumption or use another
  clock.
- The WM is simulated. Real `view()`, `send_to()`, `move_to()`, scratchpad and
  reload paths are not wired. This is a 3-I item.
- Member set: the transaction must list only *viewable* outgoing clients. In
  monocle, hidden clients are already unmapped. If the WM lists every client
  with `c->ws == old`, every monocle switch bypasses with `unnamed`. 3-I item.
- Stacking order: the scene records no stacking order. Not covered. 3-P2 needs
  it to draw the members.
- Stale generations: only the headless test covers them. The Xvfb A→B→A case
  is a smoke test.
- Not covered yet: compositor-owner replacement (`_DANIWM_TEST_WS_COMP_V1`
  change) while a scene is held, and forced partial X errors during retention.
- Content isolation during a live crossfade (remap while the scene is drawn):
  3-P2. The model frees the scene at admit.
- Single monitor only. Per-monitor clipping, negative and unequal outputs:
  3-P2.
- No rendering of the scene, no crossfade, no input checks, no stationary UI
  checks: 3-P2.
- The shared budget with close layers (exit-layer) is not combined yet. The
  prototype uses its own 32 MiB limit.
- Hardware pacing: SKIP (Xvfb).

## Next step

Pause for review. If the review accepts this result, a separate request can
start 3-P2 (input, rendering and resources, test-only).
