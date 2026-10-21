# Basic WM audit

Scope: focus, monocle, scratchpad, workspace moves, transient dialogs,
fullscreen, reload, float geometry, and test isolation. Not a full security
or compositor audit. Tests run on isolated Xvfb displays.

## Confirmed findings

### 1. Transient dialog does not follow its managed parent (medium)

Location: `src/client.c`, `manage()`.

`XGetTransientForHint()` is used only to decide floating state. The managed
parent's workspace and monitor are not inherited. `rulews` defaults to
`curws`, and `c->mon` defaults to the pointer monitor.

Reproduced: create parent on workspace index 0; switch to workspace index 1;
map a dialog with WM_TRANSIENT_FOR pointing at the parent. Dialog gets
_NET_WM_DESKTOP=1 and is viewable on the current workspace, rather than
staying with its parent. manage() also focuses the dialog.

Suggested fix: resolve the managed transient parent and inherit its workspace
and monitor, with an explicit precedence policy for configured workspace rules
and a supplied _NET_WM_DESKTOP. Do not automatically switch workspaces to show
an off-workspace dialog.

### 2. Test suite can kill terminals outside the test display (high, test safety)

Locations: `test/test-workspaces.sh` lines 51/70 and
`test/test-tasklist.sh` line 71.

`pkill -x xterm` is not restricted by DISPLAY. It may terminate the user's
real xterm sessions. This is a static, unambiguous finding; the destructive
commands were NOT executed. The make check run was stopped before these
suites.

Suggested fix: record and terminate test-owned PIDs, or use client IDs scoped
to the test display. Avoid process-name-wide cleanup.

## Validation

Passed complete individual suites:
- test-focus-model.sh
- test-noinput.sh
- test-takefocus.sh
- test-remap-steal.sh
- test-direct-state.sh
- test-reload-hidden.sh
- test-fs-offws.sh
- test-clamp-float.sh
- test-scratch-focus.sh

The initial make check was intentionally interrupted; it is NOT a full-suite
pass. verify.sh printed passes for its early layout/workspace/fullscreen/
scratchpad checks before interruption.

Corrected the new scratchpad regression test: clients are appended by attach(),
not prepended. It now explicitly selects the newer, non-head client. The
corrected test passes for first spawn, repeated toggle, preserving the tiled
window under the scratchpad, and closing the saved return target.

## Remediation

Both findings are fixed:

- `src/client.c`: managed transient parents supply the default workspace and
  monitor. Workspace precedence is valid explicit `_NET_WM_DESKTOP` > configured
  rule > managed parent > current workspace. Parked scratchpad workspaces are
  not inherited. Off-workspace dialogs stay hidden and do not steal focus.
- `test/test-workspaces.sh` and `test/test-tasklist.sh`: replaced global
  `pkill -x xterm` with X client cleanup scoped to the dedicated test DISPLAY,
  including unmapped clients. No process-name-wide cleanup remains there.
- Added `test/test-transient-parent.py` / `.sh` and registered the suite in
  `test/run-all.sh`. Covers off-workspace/nested dialogs, rule overrides,
  explicit desktop overrides, and unmanaged-parent fallback. Monitor inheritance
  is implemented but has not been exercised on a multi-monitor test server.

Post-fix validation: build, transient-parent, scratch-focus, workspaces, and
 tasklist suites pass. Full `make check` has not been rerun after these fixes.
