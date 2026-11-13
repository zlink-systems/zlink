Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 new binding reference for all four languages (C2·C3 off, SCHED_OTHER).

No code changes, no commits, never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (MEASUREMENT METHOD, SCHEDULING CHECK, BINDING CONDITIONS, LOCK TIME LIMIT), aa-noise4/report.md.

## Why
Since 17:07 UTC the quiet guard disables deep C-states C2/C3 during measurement holds and keeps the measured processes SCHED_OTHER. Earlier binding numbers were measured with C-states on and are no longer the reference (moved to binding-daily/pre-cstate/).

## Do
For each language use that language's existing bench build and tools (copy into binding-rebase/tools/<lang>/):
- Java: worktree /root/worktree/zlink-v21-s033/lad-java, tools lad-java-m1/tools (implementation zlink-java), warmup 10 s
- .NET: worktree /root/worktree/zlink-v21-s033/lad-dotnet, tools lad-dotnet-m1/tools or lad-dotnet-l1b/tools (raw binding), warmup 10 s
- Node: worktree /root/worktree/zlink-v21-s033/lad-node, tools lad-node-m1/tools (zlink-node), warmup 3 s
- C++: worktree /root/worktree/zlink-v21-s033/l3868, tools l3868-m1/tools or l3868-l1/tools; the build-l3868 binaries must exist and be newer than sources (marker /root/jobs/fx/cx/v21/s033/l3868-l1/raw/BUILD_DONE with rc=0). If the C++ build is not ready, do the other three and say so.
Check each build is newer than its sources before taking the lock (do not rebuild unless stale).
One measure.lock hold per language (each ≤ 10 min, never > 30): binding only, 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K) × 1 run, active 5 s, source CPU 0-7, target CPU 8-15. Apply the SCHEDULING CHECK every run, and also record /sys/devices/system/cpu/cpu{0,8}/cpuidle/state{2,3}/disable at run start (must be 1).
Write binding-daily/<lang>-<YYYY-MM-DD UTC>.json per language: conditions "C2·C3 off, SCHED_OTHER, guard v5", measured_utc, and per cell msg/s + raw path.

## Output
binding-rebase/report.md (Korean, one table: language × 6 cells, raw cited; plus old pre-cstate value next to it where it exists). Append after each language. Time cap 2 h.
