Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Java ladder — re-measure L4 and L5~L7 after the admission fix (jl57bp commit cffa8009d3). MEASUREMENT ONLY, NO CODE CHANGES.

Worktree /root/worktree/zlink-v21-s033/lad-java-l57p, branch exp/4350-java-ladder-l57p, tip cffa8009d3 (bench(java): preserve ladder request admission). jl57bp/report.md lists what it built; if any client/server install is older than its source, rebuild only the bench client/server (no Framework rebuild needed; the change is bench-only FrameworkStack.java) and check rc 0 and binary newer than source before measuring.
Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Do not touch other worktrees.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all of it; A/A gate is closed), ladder-steps.md, lad-java-s3/report.md and its tools (reuse them pointed at this tip), jl57bp/report.md.

Measure in ONE exclusive hold of /root/jobs/fx/cx/measure.lock (at most 30 min; wait your turn): L4 and L5~L7, 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), warmup 10 s, active 5 s, one discarded warm run per cell change, then counted L4, L5_7 (1 each per cell; if a cell's L5~L7 loss is within 3 points of 21%, i.e. 18-24%, run 3 counted of both and use medians). Pin source 0-5 / target 8-13; record SCHED policy and Cpus_allowed_list of every thread at start and mid-active. Every counted run must have errors 0 and submitted = completed = received; otherwise mark the cell invalid, keep raw, and report it.
Binding: binding-daily/java-2026-11-13b.json.

## Output
/root/jobs/fx/cx/v21/s033/lad-java-s4/report.md (Korean, short): per cell L4 and L5~L7 msg/s, L5~L7÷binding, L5~L7 loss vs L4 with pass/fail against 21%, errors/submitted/completed/received, raw path per number, affinity/scheduling evidence. Time cap 1.5 h.
