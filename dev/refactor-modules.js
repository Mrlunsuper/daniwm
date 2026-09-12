// Sequential module split for daniwm.c.
// Authoritative docs live in the repo: TASKS.md (phases), GAPS_PLAN.md (decisions),
// refactor_plan.md (module map). Children read them; this script only sequences work.
//
// One writer per cwd => strictly sequential (real dependency chain: state.h -> modules -> link).
// Each writer child is gated on `make src-check` (compile-only, 0 warnings) + `make`
// (legacy binary from daniwm.c must keep building until the final phase).

const DOCS = [
  "Repo: /home/dani/daniwm. Branch: refactor/modules. Baseline commit: 82580e6.",
  "Before editing, READ these files (they are the task spec):",
  "  - TASKS.md        -> the phase named below (exact line ranges, per-file checklist)",
  "  - GAPS_PLAN.md    -> decisions + ownership matrix (section 3). WINS over refactor_plan.md on conflict.",
  "  - refactor_plan.md -> module map, for context only."
].join("\n");

const RULES = [
  "HARD RULES:",
  "1. Pure refactor: do NOT change behavior or logic. Move code verbatim.",
  "2. Do NOT rename functions/structs/typedefs.",
  "3. `static`: remove only where GAPS_PLAN.md section 3 says the symbol is public/cross-module;",
  "   keep `static` for every helper with no caller outside its own file.",
  "4. Do not reformat untouched code, do not fix unrelated issues, do not add features.",
  "5. Do not run `make check` (slow) in this phase.",
  "6. Verify: `make src-check` and `make` both succeed with ZERO compiler warnings.",
  "7. Then `git add -A && git commit -m \"<phase>: <what moved>\"`.",
  "8. Report back concisely: commit sha, files added/changed, warning count (must be 0),",
  "   and any deviation from the spec. If blocked, report the exact error instead of guessing."
].join("\n");

function step(key, label, phase, extra) {
  return runs.run(key, {
    agent: "worker",
    model: "deepseek/deepseek-v4-pro",
    context: "fresh",
    label,
    task: [DOCS, "", "PHASE: " + phase, extra || "", "", RULES].join("\n"),
    gate: "make src-check 2>&1 | grep -c 'warning:' | grep -qx 0 && make"
  });
}

const foundation = await step(
  "foundation",
  "Phase 0.5+1+2 foundation",
  "Phase 0.5 (T0.6) + Phase 1 (T1.1-T1.4) + Phase 2 (T2.1-T2.6)",
  [
    "Do, in this order:",
    "a) T0.6: add the TEMPORARY `src-check` target to Makefile exactly as in GAPS_PLAN.md section 2.",
    "   Also do G13 now: add `src/*.o` and `src/*.d` to .gitignore (objects must never be committed).",
    "b) T1: create src/types.h moving the typedefs/structs listed in TASKS.md T1.2 verbatim.",
    "c) T1.4: create SKELETON headers for ALL remaining modules",
    "   (state.h, monitor.h, sysmon.h, bar.h, ewmh.h, layout.h, client.h, mouse.h, keys.h, config.h)",
    "   with `#pragma once` and the FINAL prototypes from the ownership matrix in GAPS_PLAN.md section 3.",
    "   Only create headers here; do NOT create their .c files yet.",
    "d) T2: create src/state.h + src/state.c per TASKS.md T2.1-T2.6.",
    "   Sections 4 (G4/G5/G8) of GAPS_PLAN.md are mandatory: S() must be `static inline` in state.h,",
    "   ui_scale extern, LAYOUT/MFACT/NMASTER macros in state.h, MAXWS/MAXMONS/FLOAT_STEP/RSZ_STEP in state.h.",
    "   Keys globals (keys/nkeys/capkeys) and last_kill_win/time are NOT extern here - they stay static in",
    "   keys.c / client.c later.",
    "Do NOT touch daniwm.c yet."
  ].join("\n")
);

const sysmonMonitor = await step(
  "sysmon-monitor",
  "Phase 3+4 sysmon+monitor",
  "Phase 3 (T3.1-T3.4) + Phase 4 (T4.1-T4.4)",
  [
    "a) Create src/sysmon.h + src/sysmon.c from daniwm.c lines 431-560, per TASKS.md T3.",
    "b) Create src/monitor.h + src/monitor.c from lines 210-294 AND move `screen_extents`",
    "   (lines 1220-1233) here per TASKS.md T4.3 / GAPS_PLAN G7.",
    "c) monitor.c calls bar_style() (line 291): include bar.h (skeleton already exists).",
    "Do NOT modify daniwm.c."
  ].join("\n")
);

const ewmh = await step(
  "ewmh",
  "Phase 5 ewmh",
  "Phase 5 (T5.1-T5.4)",
  [
    "Create src/ewmh.h + src/ewmh.c from daniwm.c lines 992-1387 per TASKS.md T5.",
    "`get_strut` (1180-1218) stays `static` inside ewmh.c - do NOT export it (G12).",
    "`screen_extents` is now in monitor.c, so include monitor.h instead of defining it here.",
    "`setfullscreen` calls arrange(): include layout.h.",
    "Do NOT modify daniwm.c."
  ].join("\n")
);

const layoutClient = await step(
  "layout-client",
  "Phase 6+7 layout+client",
  "Phase 6 (T6.1-T6.3) + Phase 7 (T7.1-T7.4)",
  [
    "a) Create src/layout.h + src/layout.c from lines 295-429 per TASKS.md T6.",
    "b) Create src/client.h + src/client.c gathering FOUR regions per TASKS.md T7.2:",
    "   helpers 167-209; actions 803-990; matchrules 1389-1406 (G1, keep `static`);",
    "   manage/unmanage 1450-1537. Also move last_kill_win + last_kill_time (942-943),",
    "   keep them `static` in client.c (G10).",
    "   findscratch (1408-1418) is NOT client.c - it goes to keys.c in a later phase.",
    "Do NOT modify daniwm.c."
  ].join("\n")
);

const bar = await step(
  "bar",
  "Phase 8 bar",
  "Phase 8 (T8.1-T8.5)",
  [
    "Create src/bar.h + src/bar.c from lines 561-802, PLUS move `bar_style` (2267-2317) and",
    "`scaled_font_pat` (2246-2265) here per TASKS.md T8.3 / GAPS_PLAN G6.",
    "bar.h exports ONLY `bar_style` and `drawbar`.",
    "Everything else (bar_text, bar_textw, bar_runs, utf8_*, get_title, xft_alloc,",
    "bar_glyph_font, scaled_font_pat) stays `static` in bar.c.",
    "Include sysmon.h (sys_cpu/mem/bat/vol) and client.h (ws_occupied).",
    "Do NOT modify daniwm.c."
  ].join("\n")
);

const mouseKeys = await step(
  "mouse-keys",
  "Phase 9+10 mouse+keys",
  "Phase 9 (T9.1-T9.4) + Phase 10 (T10.1-T10.7)",
  [
    "a) Create src/mouse.h + src/mouse.c from lines 1539-1638 per TASKS.md T9.",
    "b) Create src/keys.h + src/keys.c per TASKS.md T10:",
    "   - Key table: `static Key *keys; static unsigned nkeys, capkeys;` INSIDE keys.c.",
    "   - Move push_key_fn (1766-1774) and add_default_keys (1831-1872) from config.c into keys.c (G3/G11).",
    "   - Add `void keys_reset(void);` to keys.h; config.c will call it instead of `nkeys = 0`.",
    "   - Move grabkeys (2231-2244), findscratch (1408-1418, keep `static`), k_scratch (1420-1448),",
    "     all k_* (1648-1743), actions[] (1728-1743), vol argv (1675-1686), FLOAT_STEP/RSZ_STEP.",
    "   - Declare `void k_reload(int);` in keys.h (impl stays in config.c). keys.h must NOT include config.h (G15).",
    "Do NOT modify daniwm.c."
  ].join("\n")
);

const config = await step(
  "config",
  "Phase 11 config",
  "Phase 11 (T11.1-T11.4)",
  [
    "Create src/config.h + src/config.c from lines 1745-2230 per TASKS.md T11.",
    "push_key_fn and add_default_keys are ALREADY in keys.c (moved last phase) - do not duplicate them.",
    "config_defaults/load_config must call keys_reset() from keys.h instead of writing nkeys directly.",
    "Implement k_reload here (it is declared in keys.h).",
    "Keep the runtime config file name/paths identical; repo-root file `config` is unrelated to src/config.c.",
    "Do NOT modify daniwm.c."
  ].join("\n")
);

const finalize = await runs.run("finalize", {
  agent: "worker",
  model: "deepseek/deepseek-v4-pro",
  context: "fresh",
  label: "Phase 12+13+14 finalize",
  task: [
    DOCS,
    "",
    "PHASE: Phase 12 (T12.1-T12.3) + Phase 13 (T13.1-T13.5) + Phase 14 (T14.1-T14.7)",
    "This is the phase that finally links everything and runs the real test suite.",
    "",
    "a) Create src/main.c from lines 2319-2619 (xerror_* handlers + main()). Include every needed header.",
    "b) Rewrite Makefile: replace the TEMPORARY src-check target with the real SRCS/OBJS rules for",
    "   all 11 modules, add -MMD -MP and `-include $(OBJS:.o=.d)`, keep the binary at the REPO ROOT",
    "   (test/*.sh call $TDIR/../daniwm), clean must remove src/*.o and src/*.d (G16).",
    "c) `git rm daniwm.c` - the split is now the only source of truth.",
    "d) Fix any link errors that surface (this is the first real link).",
    "e) Verify HARD before committing:",
    "   `make clean && make` -> ZERO warnings (compare against baseline build log if useful)",
    "   `make check`         -> must print 'RESULT: ALL PASS'",
    "   `size daniwm`        -> .text within +-5% of baseline 69062 bytes",
    "f) Commit.",
    "",
    "HARD RULES:",
    "1. Pure refactor: no behavior/logic changes; fix only what the build/link requires.",
    "2. Do NOT rename functions/structs/typedefs.",
    "3. If `make check` fails, report the failing test output verbatim; do NOT weaken or edit tests.",
    "4. Report: commit sha, warning count, make check result, .text size, and every deviation."
  ].join("\n"),
  gate: "make clean >/dev/null 2>&1 && make 2>&1 | grep -c 'warning:' | grep -qx 0 && make check 2>&1 | grep -qx 'RESULT: ALL PASS'"
});

const audit = await runs.run("audit", {
  agent: "reviewer",
  model: "deepseek/deepseek-v4-pro",
  context: "fresh",
  label: "Independent audit",
  task: [
    "Read-only audit of a completed refactor in /home/dani/daniwm (branch refactor/modules).",
    "The refactor split monolithic daniwm.c (2619 lines) into src/ modules. Baseline commit: 82580e6.",
    "",
    "Inspect the full diff with `git diff 82580e6..HEAD` and `git show --stat` (you have bash).",
    "Cross-check against TASKS.md acceptance criteria and the ownership matrix in GAPS_PLAN.md section 3.",
    "",
    "Look specifically for:",
    "1. Behavior changes smuggled into the move (logic edits, reordered statements, changed defaults).",
    "2. Renamed functions/structs/typedefs (forbidden).",
    "3. `static` removed on a helper with no external caller, or kept on a cross-module symbol.",
    "4. Missing/duplicated definitions (check the ownership matrix).",
    "5. Header cycles, especially keys.h/config.h (G15).",
    "6. Object files or other build artifacts committed (G13).",
    "7. `daniwm.c` still present, or binary moved out of repo root (G16).",
    "8. Any TASKS.md checklist item silently skipped.",
    "",
    "Report concrete findings with file:line and severity P0/P1/P2. Do not fix anything.",
    "End with exactly one of: `Merge verdict: BLOCK`, `Merge verdict: OK`, `Merge verdict: OK with notes`."
  ].join("\n")
});

const results = [foundation, sysmonMonitor, ewmh, layoutClient, bar, mouseKeys, config, finalize, audit];
return {
  steps: results.map(function (r) {
    return { runId: r.runId, ok: r.ok, output: r.output };
  }),
  auditVerdict: audit.output
};
