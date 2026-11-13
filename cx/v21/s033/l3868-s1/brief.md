Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: C++ ladder — build, then binding reference + step measurement L0..L5~L7 under the CURRENT conditions (quick method). NO CODE CHANGES.

Worktree /root/worktree/zlink-v21-s033/l3868-next, branch exp/3868-cpp-ladder-next, tip 62e766f88d (L2 fbdcb16e26, L3 dd83e75df4, L4 276b84f248, L5~L7 62e766f88d; l3868-impl2/report.md, l3868-impl3/report.md). Steps are selected in one build by ZLINK_CPP_LADDER_LEVEL=0/1/2/3/4/7 (7 = L5~L7). Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files (yours or earlier jobs').
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (ALL of it — MEASUREMENT METHOD, WARM RUN, LOCK TIME LIMIT 30 min, SCHEDULING CHECK, CPU PINNING, BINDING CONDITIONS, REPORTING), ladder-steps.md, ladder-status.txt.

## Build (outside the lock)
build-l3868-next was deleted by l3868-impl3. Rebuild with l3868-impl3/tools/configure-build-inner.sh via /root/jobs/fx/cx/cpp-build-slot.sh (Release, USE_BINDINGS_SOURCE=ON, Core prefix 1.25.0, -j3, ≤ 25 GB). Chain: build && verify every bench binary is newer than its sources (rc 0, not 147) && only then measure. Also make sure the C++ binding server/client binaries used are built from the same tree.

## Measure (6 cells = send-saturation, request-serial, request-backpressure × 1K, 4K; warmup 3 s, active 5 s)
Reusable runner tools with scheduling/affinity records: /root/jobs/fx/cx/v21/s033/binding-rebase/tools/cpp/ — copy to your tools dir and make them pin source 0-5 / target 8-13 themselves.
1. Binding (raw C++ binding), 6 cells × 1 counted run, one discarded warm run per cell change. Write /root/jobs/fx/cx/v21/s033/binding-daily/cpp-2026-11-13b.json (conditions: C2·C3 off, SCHED_OTHER, pin 0-5/8-13, guard v8).
2. Framework ladder L0, L1, L2, L3, L4, L5~L7 (levels 0,1,2,3,4,7), 6 cells × 1 counted run each, warm run per cell change. 5-9% step loss in a cell → 3 runs in that cell (L5~L7 vs L4: budget 21%, 3 runs when 19-23%).
3. One hold per binding or per one or two steps, never more than 30 min per hold; take /root/jobs/fx/cx/measure.lock exclusive only while measuring. Other measurement jobs (Java, Node) share the lock; wait your turn.
Every counted run records SCHED policy and Cpus_allowed_list of every source/target thread at start and mid-active; invalid runs are repeated, never used.

## Output
/root/jobs/fx/cx/v21/s033/l3868-s1/report.md (Korean, short): table A cell × (binding, L0..L5~L7 msg/s, L5~L7÷binding); table B step losses per cell with pass/fail (L0 ≥ 0.95×binding, steps ≤ 7%, L5~L7 ≤ 21%); raw path per number; affinity/scheduling evidence. Keep build-l3868-next after the job (measurements will continue). Time cap 5 h.
