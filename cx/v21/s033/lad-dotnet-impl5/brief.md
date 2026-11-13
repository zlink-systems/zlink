Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: .NET ladder — implement L5 owner queue·worker dispatch (#4355) (and then L6 #4356 / L7 #4357 if time remains). IMPLEMENTATION ONLY, NO MEASUREMENT. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-dotnet, branch exp/4350-dotnet-ladder, HEAD a7fd48d1b1. Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (PARITY, FIRST OUTPUT call-shape, build rules), /root/jobs/fx/cx/v21/s033/issues-4351-4357.md (sections #4355, #4356, #4357), the previous step's report and call-shape of this language (listed below), and for reference the other languages' reports for the same step where they exist.
Previous step: lad-dotnet-m1/report.md and call-shape.md (L0 header fix ab21f1eb31, L2 envelope header 2590b2a030). Java L3 reference: commit 23be466e71 on exp/4350-java-ladder (git show it).

## Why
Measurement is paused while the server noise is fixed (A/A over 5%). Implementation must not wait: when measurement reopens, only the previous and new step's 6 cells need measuring.

## Rules
- Do NOT take /root/jobs/fx/cx/measure.lock and do NOT run any benchmark or measurement. Builds and unit/contract tests of the bench are fine (the quiet guard pauses them during other jobs' measurements; just wait).
- Restore ONLY the features of that step, using the real production Framework code path for that feature (trace production call sites with file:line), on top of the previous step, behind a run-time switch (env or flag) so all steps run from one build. Keep receive/reply parity for everything not restored.
- One commit per step. Before each commit: build, binaries newer than sources, and a quick functional check that the bench server answers (a single short smoke run of the bench WITHOUT the measure lock is NOT allowed — use the existing unit/contract tests or a dry start/stop that sends no load).
- Write /root/jobs/fx/cx/v21/s033/lad-dotnet-impl5/call-shape-<step>.md: binding vs new step, four paths (source send, target receive, target reply, source completion), file:line both sides, and which restored feature each difference belongs to. Any difference not belonging to a restored feature must be fixed before committing.

## Output
/root/jobs/fx/cx/v21/s033/lad-dotnet-impl5/report.md (Korean, short): per step — restored feature, production path file:line, bench code file:line, commit, build/test evidence, call-shape summary, what to measure when measurement reopens (previous vs new step, 6 cells). Time cap 4 h.

## Previous steps and PM decision (11-13 17:38)
- L3 b8eb5589e7 and L4 a7fd48d1b1 are done (lad-dotnet-impl3/report.md, call-shape-L3.md, call-shape-L4.md).
- If the production owner-queue entry cannot be reached without also bringing in L6 activation and L7 operation registry, do NOT add a bench-only access point to production code: implement L5~L7 together as ONE step (one commit, one run-time switch, call-shape-L5-7.md), measured later against a 21% budget. If .NET can separate them cleanly, keep one step per commit as before.
