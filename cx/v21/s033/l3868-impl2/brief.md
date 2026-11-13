Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: C++ ladder — implement L2 envelope header (#4352) (and then L3 service wire framing (#4353) if time remains). IMPLEMENTATION ONLY, NO MEASUREMENT. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/l3868-next, branch exp/3868-cpp-ladder-next, HEAD a67753ede4. Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (PARITY, FIRST OUTPUT call-shape, build rules), /root/jobs/fx/cx/v21/s033/issues-4351-4357.md (sections for L2 envelope header (#4352) and L3 service wire framing (#4353)), the previous step's report and call-shape of this language (listed below), and for reference the other languages' reports for the same step where they exist.
Previous step: l3868-m1/report.md, l3868-l1/call-shape.md (L1 permit 608513f260). This is a separate worktree so the measured build in /root/worktree/zlink-v21-s033/l3868 (build-l3868, L0/L1) stays untouched. C++ builds only via /root/jobs/fx/cx/cpp-build-slot.sh with a copy of l3868-l1/tools/configure-build-inner.sh pointed at THIS worktree and build dir build-l3868-next (Release, USE_BINDINGS_SOURCE=ON, Core prefix 1.25.0, -j3, ≤ 25 GB). Java/.NET L2 references: exp/4350-java-ladder bf83275257, exp/4350-dotnet-ladder 2590b2a030.

## Why
Measurement is paused while the server noise is fixed (A/A over 5%). Implementation must not wait: when measurement reopens, only the previous and new step's 6 cells need measuring.

## Rules
- Do NOT take /root/jobs/fx/cx/measure.lock and do NOT run any benchmark or measurement. Builds and unit/contract tests of the bench are fine (the quiet guard pauses them during other jobs' measurements; just wait).
- Restore ONLY the features of that step, using the real production Framework code path for that feature (trace production call sites with file:line), on top of the previous step, behind a run-time switch (env or flag) so all steps run from one build. Keep receive/reply parity for everything not restored.
- One commit per step. Before each commit: build, binaries newer than sources, and a quick functional check that the bench server answers (a single short smoke run of the bench WITHOUT the measure lock is NOT allowed — use the existing unit/contract tests or a dry start/stop that sends no load).
- Write /root/jobs/fx/cx/v21/s033/l3868-impl2/call-shape-<step>.md: binding vs new step, four paths (source send, target receive, target reply, source completion), file:line both sides, and which restored feature each difference belongs to. Any difference not belonging to a restored feature must be fixed before committing.

## Output
/root/jobs/fx/cx/v21/s033/l3868-impl2/report.md (Korean, short): per step — restored feature, production path file:line, bench code file:line, commit, build/test evidence, call-shape summary, what to measure when measurement reopens (previous vs new step, 6 cells). Time cap 4 h.
