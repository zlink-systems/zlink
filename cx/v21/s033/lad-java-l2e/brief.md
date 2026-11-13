Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Java ladder L2 — add the REPLY envelope (encode at target reply, completion header parse at source), then replay the later steps on top. IMPLEMENTATION ONLY, NO MEASUREMENT. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java, branch exp/4350-java-ladder. Current L2 commit: bf83275257. Branch tip: 4bf0f1c4bf (L5~L7). Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Do NOT take /root/jobs/fx/cx/measure.lock and do NOT run any benchmark; builds and unit/contract tests only.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md, /root/jobs/fx/cx/v21/s033/ladder-status.txt (step definition table), the C++ L2 reference: l3868-impl2/report.md and git show fbdcb16e26 in /root/worktree/zlink-v21-s033/l3868-next (C++ L2 restores request AND reply envelope: reply via production route_packet_dispatcher.cpp:444, completion header parse once). Previous Java L2: commits 5d74ddac1e, e32f4ff127, bf83275257 (lad-java-l2b/report.md; the older lad-java-l2r/ directory is an earlier unrelated job, ignore it). Use the LAST of those three (bf83275257) as the L2 commit to build on, Later steps: parity 6b77f3b04d, L3 23be466e71 (on top of WIP c20022b27e), L4 18a25db605, L5~L7 4bf0f1c4bf (lad-java-impl5/report.md, call-shape-L5-7.md).

## Why (PM 11-13 18:05)
L2 = envelope header on BOTH request and reply, as production does. Java L2 restored only the request side; the reply-side envelope must be added so all four languages measure the same step.

## Do
1. Create the fix on top of the L2 commit: git switch -c tmp-l2-reply bf83275257. Add the production reply envelope encode on the target reply path and the production completion header parse (once) on the source, under the same L2 run-time switch. Trace production file:line for both. Build + tests; commit "bench(Java): restore reply envelope in ladder L2 (#4352)".
2. Replay the later steps: git rebase --onto tmp-l2-reply bf83275257 exp/4350-java-ladder (non-interactive). Resolve conflicts so each later step keeps its own feature; where a later step already uses the full production path (e.g. L5~L7 through the public Framework route), keep that. After the rebase, build and run the bench unit/contract tests at the tip; if a middle commit no longer builds, fix it in that commit (git rebase -x "<build cmd>" is fine). Delete tmp-l2-reply after.
3. Update call-shape: append the reply-side rows (target reply, source completion) with file:line to the L2 call-shape file of this language, and note in report.md the old→new commit ids of every replayed step.

## Output
/root/jobs/fx/cx/v21/s033/lad-java-l2e/report.md (Korean, short): reply envelope production path file:line, bench file:line, new L2 commit, old→new commit map, build/test evidence. Time cap 3 h.
