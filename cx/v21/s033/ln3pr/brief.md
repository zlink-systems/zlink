Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Pre-merge A/B for PR #4415 (Node service wire frame written into native storage) — measure Node ladder L2 and L3 on current main vs the PR branch, same hold, 6 cells. MEASUREMENT ONLY, NO PRODUCTION CODE CHANGES.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Do not touch other worktrees.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all of it, incl. BUILDS BETWEEN HOLDS; A/A gate closed), ladder-steps.md, lad-node-s1/report.md + tools (method, runner, L2/L3 switches), lad-node-s2/report.md.

## Setup (you create these two worktrees)
- A (baseline): `git -C /root/zlink worktree add /root/worktree/zlink-v21-s033/ln3-base origin/main` (6350275fac or later; record the commit), then `git apply /root/jobs/fx/cx/v21/s033/ln3pr/ladder-bench.patch` (the ladder bench at exp/4350-node-ladder c019495a82; bench files only).
- B (PR): `git -C /root/zlink worktree add /root/worktree/zlink-v21-s033/ln3-pr origin/perf/4353-node-service-wire` (afd3a0cea4 = main 6350275fac + the PR commit), same patch.
- Build both the way lad-node-s1 built (production packages from that worktree's framework/languages/node, Core binding 1.25.0 tgz from /root/.cache/zlink/packages/local-1.25.0/node/npm/). Check rc 0 and outputs newer than sources. Record per worktree: HEAD, `git diff --stat` (bench only), and SHA256 of the built framework dist files that the PR touches (service-wire-m6a-codec.js, raw-service-mesh-runtime.js, node-raw-binding-port.js) — A and B must differ there and only there.

## Measure
Exclusive holds of /root/jobs/fx/cx/measure.lock, at most 10 min each. Steps L2 and L3 (same switches as lad-node-s1), 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), warmup 10 s, active 5 s, one discarded warm run per cell change, then counted runs in the order A-L2, B-L2, A-L3, B-L3 per cell, 1 each; if |B-L3 vs A-L3| is within 3-9% for a cell, add 2 more counted A-L3/B-L3 pairs and use medians. Pin source 0-5 / target 8-13; record SCHED policy and Cpus_allowed_list of all threads at start and mid-active. Every counted run: errors 0 and submitted = completed = received, else invalid (keep raw, report).

## Output
/root/jobs/fx/cx/v21/s033/ln3pr/report.md (Korean, short): per cell A-L2, B-L2, A-L3, B-L3 msg/s; L3 loss vs L2 for A and B (the step loss the PR targets) and B-L3 / A-L3; verdict per cell: PR helps / neutral (within ±3%) / hurts; raw path per number; affinity/scheduling evidence; the build fingerprints. Time cap 1.5 h. Remove the two worktrees at the end only after copying the build fingerprints into the report (keep raw).
