Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Node ladder — measure L4 and L5~L7 after the handler DI fix (node-l57g commit c019495a82). MEASUREMENT ONLY, NO CODE CHANGES.

Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder, tip c019495a82 (bench(node): fix ladder L5_7 handler metrics DI). node-l57g/report.md says what it built and ran (run-six-smoke.sh); if any built output is older than its source, rebuild only what is stale and check rc 0 and output newer than source before measuring.
Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Do not touch other worktrees.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all of it, incl. BUILDS BETWEEN HOLDS; A/A gate is closed), ladder-steps.md, lad-node-s1/report.md and its tools (reuse them), node-l57g/report.md.

Measure in exclusive holds of /root/jobs/fx/cx/measure.lock (at most 10 min each; wait your turn): L4 and L5~L7 (ZLINK_LADDER_L5_7_SERVER=1 ZLINK_LADDER_L5_7_CLIENT=1 as node-l57g used), 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), warmup 10 s, active 5 s, one discarded warm run per cell change, then counted L4, L5_7 (1 each per cell; if a cell's L5~L7 loss is within 18-24%, run 3 counted of both and use medians). Pin source 0-5 / target 8-13; record SCHED policy and Cpus_allowed_list of every thread at start and mid-active. Every counted run must have errors 0 and submitted = completed = received; otherwise mark the cell invalid, keep raw, and report it.
Binding: the binding numbers in lad-node-s1/report.md (same day, same binding build).

## Output
/root/jobs/fx/cx/v21/s033/lad-node-s2/report.md (Korean, short): per cell L4 and L5~L7 msg/s, L5~L7÷binding, L5~L7 loss vs L4 with pass/fail against 21%, errors/submitted/completed/received, raw path per number, affinity/scheduling evidence. Time cap 1.5 h.
