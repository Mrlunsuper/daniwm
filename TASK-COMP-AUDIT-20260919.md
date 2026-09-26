# dani-comp Audit Tasks

Source: `AUDIT-X11-COMPOSITOR.md` (2026-09-19)

## 1. Executive Plan

Fix strategy has three phases.

Phase 1 cleans up resources. Free server-side objects on exit (`shadow_fill`, `present_gc`, `dmg_scratch`). Add `XCloseDisplay`. These are trivial, zero-risk, and make the exit path clean.

Phase 2 improves error handling. Replace global `xerror_ignore` with trapped-error infrastructure (reuse WM's `xerr.c` pattern). Log non-BadWindow errors. This makes future debugging possible.

Phase 3 is optional polish. Increase alpha cache, reuse stack buffer, add XPresent if vsync is desired.

No task changes rendering behavior. No task changes frame scheduling. No task touches the damage pipeline or window lifecycle.

## 2. Dependency Graph

```
T-C1 (free shadow_fill)  ──┐
T-C2 (free present_gc)  ───┤
T-C3 (free dmg_scratch) ───┼──> T-C4 (XCloseDisplay) ──> T-REG-COMP (regression lock)
                            │
T-E1 (trap infra for comp)──┘

T-P1 (alpha cache) ── independent
T-P2 (stack reuse) ── independent
T-P3 (XPresent)    ── independent, optional
```

## 3. Tasks

### T-C1: Free `shadow_fill[]` on exit

**Severity**: LOW  
**File**: `src/comp.c`  
**What**: Add cleanup loop before `XCompositeUnredirectSubwindows` in both exit paths (quit_req and SelectionClear). Loop `i=0..2`: if `shadow_fill[i] != None` then `XRenderFreePicture(dpy, shadow_fill[i])`.  
**Test**: Run `test-comp.sh`. Verify no X errors. Check with `DANI_COMP_DEBUG=1` that exit is clean.  
**Estimated effort**: 10 min  

### T-C2: Free `present_gc` on exit

**Severity**: LOW  
**File**: `src/comp.c`  
**What**: Add `if (present_gc != None) XFreeGC(dpy, present_gc);` before `XCompositeUnredirectSubwindows` in both exit paths.  
**Test**: Run `test-comp.sh`.  
**Estimated effort**: 5 min  

### T-C3: Free `dmg_scratch` on exit

**Severity**: MEDIUM  
**File**: `src/comp.c`  
**What**: Add `if (dmg_scratch != None) XFixesDestroyRegion(dpy, dmg_scratch);` before `XCompositeUnredirectSubwindows` in both exit paths.  
**Test**: Run `test-comp.sh`.  
**Estimated effort**: 5 min  

### T-C4: Add `XCloseDisplay` on exit

**Severity**: LOW  
**File**: `src/comp.c`  
**What**: Add `XCloseDisplay(dpy);` after `XFlush(dpy)` in both exit paths (quit_req and SelectionClear), before `return 0`. This flushes pending requests and frees client-side resources.  
**Test**: Run `test-comp.sh`. Verify no double-free or use-after-close.  
**Estimated effort**: 5 min  

### T-E1: Compositor trapped-error infrastructure

**Severity**: MEDIUM  
**File**: `src/comp.c`  
**What**: Replace `XSetErrorHandler(xerror_ignore)` with a dual-mode handler:
- For specific operations (XDamageCreate, XCompositeNameWindowPixmap, XGetWindowAttributes): use trap/untrap pattern
- For the rest: ignore BadWindow, log everything else

Options:
1. Copy `xerr.c`/`xerr.h` pattern into comp.c (simplest, comp is a separate binary)
2. Or: keep `xerror_ignore` but add `fprintf(stderr, ...)` for non-BadWindow errors even outside debug mode

Minimum viable: just log non-BadWindow errors at all times (not just debug mode).  
**Test**: Start compositor, trigger a non-BadWindow error (e.g., invalid pixmap), verify it's logged. Run `test-comp.sh` to verify no regressions.  
**Estimated effort**: 30 min  

### T-P1: Increase alpha cache to 8 slots

**Severity**: LOW  
**File**: `src/comp.c`  
**What**: Change `#define ALPHA_SLOTS 4` to `#define ALPHA_SLOTS 8`. This reduces cache misses during fade-in animations.  
**Test**: Run `test-comp.sh` with `--fade --fade-ms 500`. Verify smooth fade.  
**Estimated effort**: 2 min  

### T-P2: Reuse stack cache buffer

**Severity**: LOW  
**File**: `src/comp.c`  
**What**: In `repaint()`, when `stack_dirty`, instead of `free(stack_cache); stack_cache = NULL; ... stack_cache = malloc(nk * sizeof(Window));`, check if `stack_nk >= nk` and reuse the existing buffer. Only realloc if `nk > stack_nk`.  
**Test**: Run `test-comp.sh`. Run `test-ws-storm.sh`.  
**Estimated effort**: 10 min  

### T-P3: Add XPresent support (optional)

**Severity**: MEDIUM (feature, not bug)  
**File**: `src/comp.c`, `Makefile`  
**What**: Add XPresent extension support for hardware vsync:
1. Check `XPresentQueryExtension` at startup
2. Create a pixmap for presentation
3. Use `XPresentPixmap` instead of `XCopyArea` for presentation
4. Handle `PresentCompleteNotify` for frame pacing
5. Fallback to XCopyArea if XPresent is unavailable

This is a significant change (~100-200 lines). Only do this if tearing is a real problem.  
**Test**: Run with `glxgears` or video playback, check for tearing. Compare CPU usage before/after.  
**Estimated effort**: 2-4 hours  

## 4. Task Matrix

| ID | Severity | Phase | Effort | Description |
|----|----------|-------|--------|-------------|
| T-C1 | LOW | 1 | 10m | Free shadow_fill[] on exit |
| T-C2 | LOW | 1 | 5m | Free present_gc on exit |
| T-C3 | MEDIUM | 1 | 5m | Free dmg_scratch on exit |
| T-C4 | LOW | 1 | 5m | Add XCloseDisplay on exit |
| T-E1 | MEDIUM | 2 | 30m | Trapped-error infrastructure |
| T-P1 | LOW | 3 | 2m | Increase alpha cache to 8 slots |
| T-P2 | LOW | 3 | 10m | Reuse stack cache buffer |
| T-P3 | MEDIUM | 3 | 2-4h | XPresent support (optional) |

## 5. Recommended Order

1. **T-C1 + T-C2 + T-C3 + T-C4** (Phase 1: cleanup, ~25 min total)
   - All independent, can be done in one commit
   - Run `make check` after

2. **T-E1** (Phase 2: error handling, ~30 min)
   - Depends on Phase 1 being done (clean exit path)
   - Run `make check` after

3. **T-P1 + T-P2** (Phase 3: performance, ~12 min total)
   - Independent of each other and of Phase 2
   - Run `make check` after

4. **T-P3** (Phase 3: XPresent, optional, 2-4h)
   - Only if tearing is a real problem
   - Significant change, needs careful testing

## 6. Non-Tasks

The following audit findings are **NOT** turned into tasks because they are design decisions, not bugs:

- **No vsync**: Documented limitation of minimal compositor
- **`win_is_opaque` depth-32 heuristic**: Safe trade-off (never incorrectly skips opaque window)
- **`shape_poll` heuristic**: Safety net for race condition, ShapeNotify handles most cases
- **`capture_from_rootmap` race**: Fallback handles it correctly (one frame of cleared background)
- **`dmg_add` bounding box**: Acceptable for typical usage, full repaint is the fallback
- **`clipbuf` never shrinks**: Trivial memory (few KB max)
- **`XRectangle` short overflow**: Only affects screens >32767px
