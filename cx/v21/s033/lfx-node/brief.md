Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: node ladder on the fix branches — step by step L2 → L3 → L4, each vs the previous step, 6 cells × 1 run. MEASUREMENT ONLY, NO PRODUCTION CODE CHANGES. Deadline: report by 00:55 UTC.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Do not touch other worktrees.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all, incl. BUILDS BETWEEN HOLDS), ladder-steps.md, and the reports + tools of lad-node-s1, lad-node-s2 (method, runner, step switches — reuse them, pointed at this worktree).

## Worktree
/root/worktree/zlink-v21-s033/lfx-node, branch exp/4350-node-fixes = origin/main 6350275fac + #4414 perf/4352-l2-bp-reply (L2), #4415 perf/4353-node-service-wire (L3), #4416 perf/4354-node-typed-json (L4), plus one local commit that adds the ladder bench (framework/bench and the ladder build switches only; patch /root/jobs/fx/cx/v21/s033/lfx-node.bench.patch). Build the way the earlier ladder jobs built it (production packages from THIS worktree; Node: Core binding 1.25.0 tgz from /root/.cache/zlink/packages/local-1.25.0/node/npm/ and the http-client local package as in pre-pr.sh lines 172-177; C++ not needed). Check rc 0 and outputs newer than sources; record HEAD and SHA256 of the built framework artifacts.

## Measure (PM 11-13 23:25 UTC method)
Exclusive holds of /root/jobs/fx/cx/measure.lock, ≤10 min each. Warmup 10 s, active 5 s, one discarded warm run per cell change. Pin source 0-5 / target 8-13; record SCHED policy and Cpus_allowed_list of all threads at start and mid-active. Every counted run: errors 0 and submitted = completed = received, else invalid (keep raw, report).
Step k (start with k = L2): for each of the 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K) counted runs of step k-1 and step k, 1 each, same hold. Loss = 1 − k/(k-1). Budget 7%. If a cell is within 5–9%, add 2 more counted pairs for that cell and use medians.
- All 6 cells within budget → next step (L3, then L4).
- Any cell over budget → STOP at this step. Do not measure higher steps. Report which cells fail and, from the step definition (ladder-steps.md) and a short profile if time allows (≤20 s, CPUs 6-7,14-15, no lock), what the step adds that costs.
Write each finished step into report.md immediately (the supervisor posts partial results).

## Output
/root/jobs/fx/cx/v21/s033/lfx-node/report.md (Korean, short): per step and cell the two msg/s, loss, pass/fail, raw path per number; the step where it stopped and why; affinity/scheduling evidence; build fingerprints. Time cap 80 min.
