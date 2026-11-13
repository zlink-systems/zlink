Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: C++ ladder — implement L4 typed payload codec (#4354), then the L5~L7 bundle (#4355~#4357: owner queue·worker, handler activation·DI scope, operation registry·completion·deadline) as ONE step. IMPLEMENTATION ONLY, NO MEASUREMENT. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/l3868-next, branch exp/3868-cpp-ladder-next, HEAD dd83e75df4 (L3). Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md, /root/jobs/fx/cx/v21/s033/ladder-steps.md (step definition table, four languages must cover the same scope), /root/jobs/fx/cx/v21/s033/issues-4351-4357.md (L4..L7 sections), /root/jobs/fx/cx/v21/s033/l3868-impl2/report.md and its call-shape-L2.md/call-shape-L3.md (previous steps of this language).
Other languages for the same steps (reference only): Java L4 18a25db605 and L5~L7 4bf0f1c4bf (exp/4350-java-ladder; lad-java-impl4/report.md, lad-java-impl5/report.md + call-shape-L5-7.md), .NET L4 a7fd48d1b1 and L5~L7 de0507ae48 (exp/4350-dotnet-ladder; lad-dotnet-impl3/report.md, lad-dotnet-impl5/report.md), Node L4 33b6c9b030 and L5~L7 3c50a02908 (lad-node-impl4c/report.md). Those branches are being rebased right now by other jobs: read with git show <commit>, never check out or touch their worktrees.
C++ builds only via /root/jobs/fx/cx/cpp-build-slot.sh using l3868-impl2's configure copy (build dir build-l3868-next, Release, USE_BINDINGS_SOURCE=ON, Core prefix 1.25.0, -j3, ≤ 25 GB).

## Rules
- Do NOT take /root/jobs/fx/cx/measure.lock and do NOT run any benchmark or load. Builds and bench unit/contract tests only (the quiet guard pauses them during other jobs' measurements; just wait).
- Restore ONLY the features of the step, using the real production Framework code path (trace production call sites with file:line), on top of the previous step, behind the same run-time switch scheme so all steps run from one build. L5~L7 bundle: go through the public Framework route (owner queue/worker, handler activation/scope, operation registry/deadline/completion) exactly as production does; no bench-only access points into Framework internals.
- One commit per step (L4, then L5_7). Before each commit: build, binaries newer than sources, bench unit/contract tests, and a dry start/stop of the server that sends no load.
- Write l3868-impl3/call-shape-L4.md and call-shape-L5-7.md: previous step vs new step, four paths (source send, target receive, target reply, source completion), file:line both sides, which restored feature each difference belongs to. Any difference not belonging to a restored feature must be fixed before committing.
- Keep raw build/test logs under l3868-impl3/raw; never delete raw files.

## Output
/root/jobs/fx/cx/v21/s033/l3868-impl3/report.md (Korean, short): per step — restored feature, production path file:line, bench file:line, commit, build/test evidence, call-shape summary, what to measure when measurement reopens (L3 vs L4, L4 vs L5_7; 6 cells each; L5_7 budget 21%). If time runs out after L4, commit L4 and report where L5_7 stands. Time cap 4 h.
