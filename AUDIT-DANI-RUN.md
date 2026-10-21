# dani-run audit

Scope: `src/run.c`, launcher config, desktop discovery/Exec,
history, calculator, keyboard/mouse and clipboard. Findings below describe the
pre-fix implementation; see Remediation status at the end for fixes.
Tests used temporary HOME/config/cache, harmless commands and isolated Xvfb.
This is not a claim of a complete security audit.

## R1 — High: IME result can write outside the stack buffer

Location: `src/run.c:1882-1890`.

`Xutf8LookupString(xic, ..., buf, 63, ..., &st)` may return a required buffer
size greater than 63 with `st == XBufferOverflow`. Code ignores `st`, then
writes `buf[n] = 0`. This is a stack out-of-bounds write; a later query copy
can also read beyond `buf` when the returned length still fits MAXQ.

Evidence: confirmed from control flow/API contract, NOT reproduced with a
real IME in this audit. No remote exploit or code execution is claimed.

Fix: handle XBufferOverflow before using length/keysym/text, allocate/retry or
discard that commit safely. Check XLookupChars/XLookupBoth as appropriate;
filter IM events and initialize keysym/status defensively.

## R2 — Medium: input-band clicks execute the first app

Locations: `src/run.c:1952-1965`, `1979-1989`.

Hit-testing divides coordinates before checking lower bounds. C integer
division truncates toward zero: `(43 - 44) / cell_height` becomes 0, not -1.
The same issue exists in drun row calculations and grid left margins.
Inter-cell gaps also count as active cells.

Reproduced in --fav with one harmless touch command: click at local y=43,
while the first cell starts at y=44, executes the command and exits.
In --power this path can invoke a configured power action accidentally;
no power actions were executed in this audit.

Fix: shared explicit rectangle hit-test for drawing, hover and click. Reject
input/footer/padding/gaps before calculating cell/row indices.

## R3 — Medium: desktop parser mixes unrelated sections

Location: `src/run.c:492-510`.

Section headers are ignored instead of tracking `[Desktop Entry]`. Values
from `[Desktop Action ...]` can supply a missing main Exec or alter the main
entry's Terminal/Hidden/NoDisplay state.

Reproduced with --list:
- Main entry without Exec, action with Exec: listed with the action command.
- Main entry with valid Exec, action with NoDisplay=true: entire app omitted.

Fix: parse keys only in the main Desktop Entry section. Validate Type and
required fields. Add action-section regression fixtures.

## R4 — Medium: desktop override/discovery behavior is incorrect

Locations: `src/run.c:514-524`, `553-560`.

Hidden entries are discarded before deduplication, and deduplication uses
visible Name rather than desktop-file ID. A user Hidden=true override cannot
mask a system app; same-name distinct apps are incorrectly collapsed.
load_drun hardcodes ~/.local/share/applications and /usr/share/applications:
XDG_DATA_HOME, XDG_DATA_DIRS and nested application directories are not read.

Reproduced:
- Temporary user zcode.desktop with Hidden=true and the installed app's name:
  system entry still listed.
- App only under temporary XDG_DATA_HOME/applications: absent from --list.

Fix: scan XDG roots in precedence order, dedupe/mask by desktop-file ID before
filtering hidden entries, recurse according to desktop-ID rules.

## R5 — Medium: Desktop Exec data is interpreted using shell expansion

Locations: `src/run.c:466-480`, `1642-1664`.

clean_exec removes every field code except %%, including meaningful %c/%k/%i.
Then launch runs wordexp with WRDE_NOCMD: command substitution is prohibited
(good), but variable expansion, globbing and tilde expansion remain active.
Desktop Exec quoting is not equivalent to shell/wordexp quoting. Literal
arguments can change; commands requiring %i/%c/%k lose arguments.

Reproduced --list for `Exec=/bin/echo %c %k %i %%`: `/bin/echo    %`.
Other expansion effects are code-confirmed, not executed as malicious payloads.

Fix: desktop-specific argv parser/field expansion; separate direct-command
and configured-command semantics. Do not introduce `/bin/sh -c` as a blanket fix.

## R6 — Medium: clipboard helper can freeze calculator before printing result

Location: `src/run.c:774-806`, activation at 1690-1692.

Despite the "never blocks" comment, to_clipboard calls blocking waitpid with
no timeout. A stuck xclip/xsel leaves the launcher running with its keyboard
grab and prevents stdout output. Non-127 failure statuses are also treated as
success, so the fallback is skipped on ordinary tool failures.

Reproduced with temporary PATH xclip stub sleeping 4 seconds: after Enter,
calculator remains running, then only prints `2` when helper exits.
No system clipboard was used.

Fix: bounded/asynchronous helper handling, reliable exec/status reporting,
release grabs/close launcher promptly, and print/flush stdout independently.

## R7 — Medium/low: calculator accepts infinity and wrong unary precedence

Locations: `src/run.c:710-727`, `760-769`.

- `exp(1000)` returns valid=1, value=inf: final result is not checked with
  isfinite. Some binary operations check finiteness, but bare functions don't.
- `-2^2` evaluates to 4 instead of conventional -(2^2) = -4 because pe_pow
  parses its base using pe_unary. Explicit `(-2)^2` should remain 4.

Reproduced by a temporary C harness including the actual run.c implementation,
compiled with AddressSanitizer/UBSan. Controls: 2^3^2=512; 1/0 and sqrt(-1)
correctly rejected. No sanitizer finding in these exercised cases.

Fix: final finite check and a precedence grammar supporting exponentiation
before unary minus while preserving 2^-2 and right associativity.

## R8 — Low: fresh-home history silently fails

Location: `src/run.c:632-650`.

Only the immediate dani-run directory is created. If ~/.cache or a custom
XDG_CACHE_HOME does not exist, mkdir/fopen fail and usage history is lost.

Reproduced using nonexistent temporary XDG_CACHE_HOME: hist_save returns but
history file does not exist. Also code-confirmed: whole-file rewrite is not
atomic/locked; simultaneous launchers may lose updates (not race-tested).

Fix: create parent directories safely; atomic replacement and appropriate
locking for shared updates; cap counts to avoid integer overflow.

## R9 — Medium: quoted # in config command is truncated

Locations: `src/run.c:156-163`, `334`.

strip_comment is not quote-aware and applies to app/power commands.
`app = ;AuditHash;/bin/echo "a#b"` loads as `/bin/echo "a`, so wordexp cannot
parse it and the launcher exits without running the intended command.

Reproduced using --fav --list and a temporary config.

Fix: a defined comment syntax with quote/escape handling, or avoid stripping
inline comments from raw command values.

## Additional review notes (not promoted to reproduced findings)

- `launch()` passes an uninitialized wordexp_t to wordfree on all errors;
  POSIX cleanup requirements differ between partial allocation and other
  failure statuses. Initialize it and clean only under the documented conditions.
- Desktop OOM path: push_entry_vec can fail, yet scan_dir unconditionally
  writes entries[nentries-1].terminal. With no prior entry this underflows.
- Both localized and plain Name allocated: unused localized allocation is
  leaked when both pointer variables are nulled. Short-lived process, low impact.
- XGrabKeyboard failure after all retries is ignored; launcher can remain
  visible without keyboard control. Not reproduced with a competing grab.
- Uppercase Ctrl+J condition tests XK_N rather than XK_J (line 1909).

## Validation / artifacts

- `make dani-run`: binary up to date.
- Temporary `/tmp/dani-run-audit.py`: --list fixture checks and Xvfb click/
  clipboard reproduction.
- Temporary `/tmp/dani-run-audit-unit.c` and binary: direct calculator/history
  checks, compiled with ASan/UBSan (non-Cairo build).
- No full GUI regression suite or real IME/multi-monitor validation performed.
- During the initial audit no production source changed and no actual power actions executed.

Recommended order: R1 (memory safety), R2 (accidental activation, especially
power), R6 (hang), R3/R4/R5 (app correctness), then R7/R8/R9. Add permanent
regression tests alongside fixes; the temporary harnesses are not registered
in make check.

## Remediation status

All R1–R9 and the additional review notes have been addressed in `src/run.c`:

- R1: validate IM status/length before indexing or copying, discard overflow
  commits safely, initialize keysym/status and call XFilterEvent.
- R2: one shared hit-test rejects bands, margins and row/cell gaps before
  division. Calculator has no activatable cells.
- R3/R4: main-section-only parsing, Type validation, ID-based precedence and
  Hidden masking, XDG data roots and recursive discovery (depth bounded, no
  directory-symlink traversal). Localized Name selection frees unused strings.
- R5: dedicated Desktop Exec tokenizer and field expansion. Arguments are
  serialized as shell-quoted literals before the existing WRDE_NOCMD launch
  path, preventing variable/glob/tilde expansion of desktop arguments. Invalid
  or embedded field codes are rejected rather than executed ambiguously.
  Config/direct commands retain their previous wordexp semantics.
- R6: print/flush the calculator result and close X before dispatching a detached
  clipboard worker; each helper has a bounded wait and nonzero exit/timeout
  triggers fallback. Detached workers do not hold caller output pipes open.
- R7: conventional exponent/unary precedence and final finite-result validation.
- R8: recursive cache-parent creation, separate advisory lock, same-directory
  temporary file plus atomic rename, saturated counts and record validation.
- R9: quote/escape-aware inline comments; quoted hashes and URL fragments survive.
- Additional notes: initialized wordexp state with status-specific cleanup;
  checked push success before indexing the desktop vector; freed localized-name
  allocations; bounded keyboard-grab failure now exits; corrected uppercase Ctrl+J.

Permanent tests: `test/run-unit.c`, `test/run-regression.py`, `test/test-run.sh`.
The suite is registered in `test/run-all.sh` / make check. It covers IM guards,
calculator, argv/fields, comments, hit testing, fresh/concurrent/saturated history,
desktop fixtures, harmless GUI clicks in fav/power, clipboard timeout/failure
fallback and competing keyboard grab. No actual power actions are used.

Validation: production build and test-run suite pass. Internal unit harness also
passes under ASan/UBSan with leak detection disabled (short-lived launcher test
retains global vectors). GUI tests use the normal Cairo-enabled binary. Real IME
commit integration and full make check have not been run for this remediation.
