Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Java ladder — re-measure ONLY L5~L7 from the fixed build (no protobuf codec registration, PM 19:00), and L4 alongside it in the same hold for the step loss. MEASUREMENT ONLY, NO CODE CHANGES.

Use the built installs of /root/worktree/zlink-v21-s033/lad-java-l57p (branch exp/4350-java-ladder-l57p, commit ee7e5c3343; lad-java-l57p/report.md lists the client/server install dirs). Do not rebuild unless a binary is older than its source. Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Never touch /root/worktree/zlink-v21-s033/lad-java (lad-java-s2 measures from it).
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all of it; A/A gate is closed), ladder-steps.md, lad-java-s2/report.md or progress.md (method, tools). Reuse lad-java-s2/tools pointed at the l57p installs; pin source 0-5 / target 8-13; record SCHED policy and Cpus_allowed_list of every thread at start and mid-active.
Measure in ONE hold (≤ 30 min; other jobs share the lock, wait your turn): L4 and L5~L7 (ZLINK_BENCH_LADDER_L4, ZLINK_BENCH_LADDER_L5_7), 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), warmup 10 s, active 5 s, one discarded warm run per cell change, then alternate L4 and L5~L7 counted runs: 2 each per cell (order L4, L5_7, L4, L5_7).
Binding: binding-daily/java-2026-11-13b.json.

## Output
/root/jobs/fx/cx/v21/s033/lad-java-s3/report.md (Korean, short): per cell L4 and L5~L7 msg/s (both runs, median), L5~L7÷binding, L5~L7 loss vs L4 with pass/fail against 21%, raw path per number, affinity/scheduling evidence. Time cap 2 h.
