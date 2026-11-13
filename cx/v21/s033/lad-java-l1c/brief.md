Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Java ladder — confirm L1 vs L0 on 6 cells (one run each). MEASUREMENT ONLY, NO CODE CHANGES.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Do not touch other worktrees.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all, incl. BUILDS BETWEEN HOLDS), ladder-steps.md, lad-java-s2/report.md (its L1 1-run values 0.831/0.880/0.890/0.864/0.968/0.972 of L0 are suspected — L2 then came back above L1 on four cells) and lad-java-s3/s4 tools.

Worktree /root/worktree/zlink-v21-s033/lad-java-l57p (tip cffa8009d3; it contains L0 and L1 switches). Check installs are newer than sources; rebuild only if not (via the same tools, no source change).

Measure in ONE exclusive hold of /root/jobs/fx/cx/measure.lock (≤10 min; wait your turn): L0 and L1, 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), warmup 10 s, active 5 s, one discarded warm run per cell change, then per cell counted L0, L1 (1 each). If a cell's L1 loss is within 5–9%, add 2 more counted L0/L1 pairs and use medians. Pin source 0-5 / target 8-13; record SCHED policy and Cpus_allowed_list of all threads at start and mid-active. Every counted run: errors 0 and submitted = completed = received, else invalid (keep raw, report).

## Output
/root/jobs/fx/cx/v21/s033/lad-java-l1c/report.md (Korean, short): per cell L0, L1 msg/s, L1 loss vs L0 with pass/fail against 7%, comparison with lad-java-s2's values, raw path per number, affinity/scheduling evidence. If L1 fails on a cell, say which L1 component the switch adds (ladder-steps.md) — no fix. Time cap 30 min.
