Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: .NET ladder — binding reference + step measurement L0..L5~L7 under the CURRENT conditions (quick method). MEASUREMENT ONLY, NO CODE CHANGES.

Worktree /root/worktree/zlink-v21-s033/lad-dotnet, branch exp/4350-dotnet-ladder, tip 29d9fca82b (already built and tested at this tip; rebuild only if a binary is older than its source, via the language build script, outside the lock). Steps: L2 69891f4446, L3 e8ee8302b0, L4 4cbe0b5316 (default typed JSON), L5~L7 29d9fca82b (lad-dotnet-l2g/report.md, lad-dotnet-l4j/report.md); select steps with the ladder compile/run-time switches documented in those reports (lad-node-l2f/report.md, lad-node-impl4c/report.md); switches ZLINK_LADDER_* (L0, L1_PERMIT, L2_ENVELOPE/L2_CLIENT, L3_SERVICE_WIRE/L3_CLIENT, L4_SERVER/L4_CLIENT, L5_7_SERVER/L5_7_CLIENT); L0R/L2R/L3R are module-load controls, not ladder steps. Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files (yours or earlier jobs').
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (ALL of it — MEASUREMENT METHOD, WARM RUN, LOCK TIME LIMIT 30 min, SCHEDULING CHECK, CPU PINNING, BINDING CONDITIONS, REPORTING), /root/jobs/fx/cx/v21/s033/ladder-steps.md, ladder-status.txt. Reusable runner/aggregation tools (with scheduling and affinity records): /root/jobs/fx/cx/v21/s033/lad-java-s1/tools/ (Java, already pin 0-5/8-13; adapt to .NET) and binding-rebase/tools/dotnet/ — copy into your own tools dir and fix them to pin 0-5 / 8-13 themselves (binding-rebase used 0-7/8-15 and was rejected for that).

## Measure (6 cells = send-saturation, request-serial, request-backpressure × 1K, 4K; warmup 10 s, active 5 s)
1. Binding (raw .NET binding server/client), 6 cells × 1 counted run, with one discarded warm run per cell change. Write /root/jobs/fx/cx/v21/s033/binding-daily/dotnet-2026-11-13b.json (conditions: C2·C3 off, SCHED_OTHER, pin 0-5/8-13, guard v8).
2. Framework ladder L0, L1, L2, L3, L4, L5~L7 at the tip build via the run-time switches, 6 cells × 1 counted run each (warm run per cell change). For a step whose loss vs the previous step is 5-9% in a cell, add runs to 3 in that cell (L5~L7 vs L4: budget 21%, so 3 runs when 19-23%).
3. One hold per binding or per one or two steps; never more than 30 min per hold (release and re-take between). Take /root/jobs/fx/cx/measure.lock exclusive only while measuring.
Each counted run records SCHED policy and Cpus_allowed_list of every source/target thread at start and mid-active; invalid runs (not SCHED_OTHER, or outside 0-5 / 8-13) are repeated, never used.

## Output
/root/jobs/fx/cx/v21/s033/lad-dotnet-s1/report.md (Korean, short):
- table A: cell × (binding msg/s, L0..L5~L7 msg/s, Framework(L5~L7)÷binding)
- table B: step losses per cell (L0÷binding, L1 vs L0, … L5~L7 vs L4) with pass/fail against 0.95 (L0) / 7% / 21%
- raw path per number, affinity/scheduling evidence per run. Time cap 4 h.

A/A gate is CLOSED (common-rules PROVISIONAL WINDOW, PM 18:05): do not hold the ladder on aa-noise5. Four other jobs share the measurement lock: wait your turn, never hold more than 30 min.
