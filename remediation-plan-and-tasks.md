# daniwm Remediation Plan & Task Breakdown (Round 5)

**Baseline Status:** `make` builds with 0 warnings; `make check` fails at `test-randr` due to environment leakage.  
**Objective:** Resolve all 12 audit findings (F01–F12), ensure 100% test pass rate (`8/8`), achieve strict ISO C11 compatibility, and strengthen ICCCM/EWMH protocol compliance without regressions.

---

## Invariants (Must Hold After Every Phase)

1. `make clean && make` compiles cleanly with 0 warnings under `-Wall -Wextra -Wpedantic -Wshadow -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE -Wformat=2 -Wformat-security`.
2. `make check` passes all suites with `RESULT: ALL PASS`.
3. No regressions in window tiling, monocle, multi-monitor (Xinerama/RandR), or system tray behavior.

---

## Phase 1 — Test Harness & Environment Isolation (F01)

Fix the test suite failure by preventing ambient configuration from leaking into headless test runs.

- [x] **T1.1: Sandbox `test/test-randr.sh`**
  - **File:** `test/test-randr.sh`
  - **Action:** Create a temporary `$HOME` (`H=$(mktemp -d)`), unset `$XDG_CONFIG_HOME`, launch `HOME=$H "${WM_BIN:-$TDIR/../daniwm}"`, and add cleanup to the `trap` handler.
- [x] **T1.2: Unify Toolchain Variables in `test/test-tray.sh`**
  - **File:** `test/test-tray.sh`
  - **Action:** Remove the redundant ad-hoc `gcc` invocation in `test-tray.sh` (or respect `${CC:-gcc}` and `${CFLAGS:-}`) since `Makefile` already compiles `test/tray-icon-helper`.
- [x] **T1.3: Verification**
  - **Command:** `make check`
  - **Expected:** `8/8` suites PASS, `RESULT: ALL PASS`.

---

## Phase 2 — EWMH Protocol & Scratchpad Integrity (F02, F06)

Eliminate scratchpad leaks and improper focus assignments.

- [x] **T2.1: Refresh `_NET_CLIENT_LIST` on Scratchpad Toggle**
  - **File:** `src/keys.c`
  - **Action:** In `k_scratch()`, call `ewmh_client_list()` when parking (`s->ws = NWS`) and when unparking (`s->ws = curws`).
- [x] **T2.2: Guard `_NET_ACTIVE_WINDOW` Against Parked Windows**
  - **File:** `src/main.c`
  - **Action:** In `case ClientMessage: if (e->message_type == A_NET_ACTIVE_WINDOW)`, if `c->ws >= NWS`, either ignore the request or unpark the scratchpad via `k_scratch(0)` instead of calling `view(NWS)` and focusing an unmapped window.
- [x] **T2.3: Add Test Assertion for Scratchpad EWMH Filtering**
  - **File:** `test/verify.sh`
  - **Action:** Assert that `xprop -root _NET_CLIENT_LIST` does *not* contain the scratchpad window ID while parked, and *does* contain it when reshown.
- [x] **T2.4: Verification**
  - Run `./test/verify.sh` and verify pass.

---

## Phase 3 — Window Management & Lifecycle (F03, F04, F07, F10)

Fix window destruction on exit, mouse resize symmetry, workspace follow transitions, and RandR viewport clamping.

- [x] **T3.1: Graceful System Tray Reparenting on Exit**
  - **File:** `src/client.c`
  - **Action:** In `quit()`, invoke `tray_enable(0)` before `XCloseDisplay(dpy)` so all docked icons are safely un-embedded and returned to the root window.
- [x] **T3.2: Pairwise Tiled Vertical Resize Symmetry**
  - **File:** `src/mouse.c`
  - **Action:** In `drag_motion()`, when `dy > 0`, restore `drag.nb_up` to `drag.nb_up0`; when `dy < 0`, restore `drag.nb_dn` to `drag.nb_dn0`; when `dy == 0`, restore both to baseline.
- [x] **T3.3: Invert Unmap/Map Order in `send_to()`**
  - **File:** `src/client.c`
  - **Action:** In `send_to()`, mirror `view()`: switch `curws`, call `arrange()` first to map new workspace windows, and then unmap windows from `old`.
- [x] **T3.4: Complete Viewport Clamping on RandR Change**
  - **File:** `src/monitor.c`
  - **Action:** In `on_monitors_changed()`, clamp floating windows against the right edge (`mons[c->mon].x + mons[c->mon].w - c->fw`) and bottom edge (`mons[c->mon].y + mons[c->mon].h - c->fh`).
- [x] **T3.5: Verification**
  - Run `./test/test-mouse.sh`, `./test/test-tray.sh`, `./test/test-workspaces.sh`, `./test/test-randr.sh`.

---

## Phase 4 — UI & Bar Rendering Polish (F08, Defensive GC Checks)

Ensure title text clipping never overflows status indicators.

- [x] **T4.1: Title Ellipsis Width Guard**
  - **File:** `src/bar.c`
  - **Action:** In `drawbar()`, check `if (avail > ew)` before truncating and drawing `…` (`ELLIPSIS`). If `avail <= ew`, skip drawing the title string.
- [x] **T4.2: Defensive GC Check**
  - **File:** `src/bar.c`
  - **Action:** In `drawbar()`, add `if (!bargc) return;` alongside `if (!barpm) return;` and `if (!barxd) return;`.
- [x] **T4.3: Verification**
  - Run `./test/verify.sh` and compile with 0 warnings.

---

## Phase 5 — ICCCM Compliance, ISO C Standards & Sysmon Safety (F05, F09, F11, F12)

Strict standard compliance, synthetic notifications, and robust metrics.

- [x] **T5.1: ICCCM §4.1.5 Synthetic `ConfigureNotify` for Tiled Windows**
  - **File:** `src/main.c` (or helper in `client.c`)
  - **Action:** When handling a `ConfigureRequest` for a tiled window that rejects the client's requested coordinates or size, send a synthetic `ConfigureNotify` event with the window's current geometry.
- [x] **T5.2: ISO C Portability (Unnamed Parameters & Labels)**
  - **Files:** `src/keys.c`, `src/config.c`
  - **Action:**
    1. Replace `(int)` with `(int unused)` across all `k_*` function definitions in `src/keys.c` and `k_reload(int unused)` in `src/config.c`.
    2. Enclose the label body at `src/config.c:320` (`have_action:`) in a block `{ size_t clen = ...; }` or place a null statement before it.
  - **Verification:** Test with `gcc -std=c11 -pedantic-errors`.
- [x] **T5.3: Sysmon Counter Underflow Guard**
  - **File:** `src/sysmon.c`
  - **Action:** In `sys_cpu()`, guard with `if (total < cpu_prev_total || idle < cpu_prev_idle || dt < di) return -1;` to protect against tick anomalies.
- [x] **T5.4: Optimize Volume Check (Reduce `popen` Pipeline Forks)**
  - **File:** `src/sysmon.c`
  - **Action:** Replace `popen("amixer ... | grep ... | head", "r")` with direct C scanning of `amixer get Master` output to avoid creating 4 processes per 2-second interval.

---

## Phase 6 — Full Verification & Documentation (Round 5 Closeout)

- [x] **T6.1: Run Full Test Suite**
  - Execute `make clean && make check`. Verify 8/8 suites pass.
- [x] **T6.2: Strict Compiler Check**
  - Compile with both `gcc` and `clang` with `-Wall -Wextra -Wpedantic -Wshadow -D_FORTIFY_SOURCE=2`.
- [x] **T6.3: Document Round 5 in `AUDIT.md`**
  - Append "Round 5 (2026-09-13)" section to `AUDIT.md` detailing all findings, rationale, and fixes.
