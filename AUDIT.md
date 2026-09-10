# daniwm Audit — Round 4

**Date:** 2026-09-11  
**Scope:** Full read of `daniwm.c` (2582 lines), `Makefile`, `config`, all test scripts, `test/dock-helper.c`, `AUDIT.md` (3 prior rounds).  
**Prior audit status:** 15 items across 3 rounds, all marked DONE. This round focuses on new findings.

---

## Overall Assessment

**Solid WM.** The prior 3 audit rounds addressed the most critical issues well. This round found 4 medium and 4 low severity issues, all fixed in the same session. Build is clean with extended hardening flags.

---

## Round 4 Findings — All Fixed

| # | Severity | Issue | Fix |
|---|----------|-------|-----|
| M2 | Medium | `bar_style()` freed X resources before `!bar` early-return check — misleading order, refactor-trap | Moved `if (!bar) return` to top of function |
| M3 | Medium | `k_gapdec`: `gap_outer -= 2` when value is 1 → -1 (negative state) | Saturating subtract: `gap_outer = gap_outer >= 2 ? gap_outer - 2 : 0` |
| M4 | Medium | `localtime()` return not null-checked before `strftime` — UB on failure | Guard added; fallback shows `--:--` |
| M8 | Medium | `detach()` called `focus()` redundantly — `unmanage()` already handles focus recovery | Removed `focus()` from `detach()`; only sets `sel = NULL` |
| L4 | Low | Test scripts used hardcoded display numbers (`:93`–`:99`) — collision risk | All scripts now source `test/find_display.sh` which picks a free display dynamically |
| L5 | Low | `make install` wrote to predictable `/tmp/daniwm.desktop` — symlink race | Piped `sed` output directly into `install -Dm644 /dev/stdin ...` |
| L6 | Low | Missing hardening flags: no PIE, no stack-protector, no RELRO, no format-security | Added `-fstack-protector-strong -fPIE -Wformat=2 -Wformat-security -pie -Wl,-z,relro,-z,now` |
| L10 | Low | `.desktop` had hardcoded personal dev path `/home/dani/daniwm/daniwm` | Changed to `/usr/local/bin/daniwm` (standard install path; overridden by `make install`) |

### Not Fixed (deferred / informational)

| # | Severity | Issue | Rationale |
|---|----------|-------|-----------|
| H1 | High (notional) | CARDINAL writes use `unsigned long` — wrong byte order on big-endian LP64 | x86_64-only project; `unsigned long` is correct for Xlib format=32 on LE; documented |
| M5 | Low | Font cursors (`XCreateFontCursor`) never freed | Acceptable: X server reclaims on disconnect; lifetime == session |
| M9 | Low | No `_NET_WM_PID` or `_NET_WM_STATE_ABOVE` | Optional per EWMH spec; out of scope for minimal WM |
| L1 | Info | `quit()` doesn't free client list / rules / argv | `exit()` reclaims process memory; X resources freed by `XCloseDisplay` |

---

## Build Verification

```
make clean && make
# → 0 warnings under:
#   -Wall -Wextra -Wpedantic -Wshadow -D_FORTIFY_SOURCE=2
#   -fstack-protector-strong -fPIE -Wformat=2 -Wformat-security
#   -pie -Wl,-z,relro,-z,now
```

---

## Prior Rounds (reference)

### Round 1 (2026-09-09) — Findings

1. **H1** Incomplete EWMH workspaces (no NUMBER/CURRENT/DESKTOP/HIDDEN) → **FIXED R2**
2. **H2** `focus()` raises above docks (layering bug) → **FIXED R2**
3. **H3** `sys_vol()` blocking event loop → **FIXED R2**
4. **M1** No monitor hotplug → **FIXED R2** (RandR live replug)
5. **M2** Strut math wrong for uneven layouts → **FIXED R2**
6. **M3** Bar text baseline hardcoded / X core fonts only → **FIXED R2** (Xft)
7. **M4** Iconify loses ws/float state → documented tradeoff
8. **M5** Cross-monitor drag stays floating → **FIXED R2**

### Round 2 (2026-09-10) — All 8 resolved + 3 new fixes

### Round 3 (2026-09-10) — 15 items fixed

1. `fork()` return checked in `spawn()` and autostart
2. `ewmh_read_desktop` uses `long *` for CARDINAL
3. `_NET_WM_STATE_HIDDEN` set on monocle-hidden windows
4. `view()` maps new ws windows before unmapping old
5. Mid-drag ws switch guard
6. ASCII fast-path in `bar_glyph_font`
7. Font OOM safety in `parse_scalar`
8. `parse_bind` tokenizer rewrite (no more fragile `strtok`)
9. `scaled_font_pat` snprintf offset fix
10. Parked scratchpad filtered from `_NET_CLIENT_LIST`
11. `_NET_WM_STATE_DEMANDS_ATTENTION` urgency tracking
12. Double-kill falls back to `XKillClient` after 2s
13. `select()` consecutive error limit (>100 → exit)
14. Explicit casts for `-Wconversion -Wsign-conversion`
15. `memset(&ev)` in `kill_client`
