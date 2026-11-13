Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 Node L0R + #4351 Node L1 — new quick measurement method. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder, HEAD c512887606: L0 dba747fe85 + 8eb3525b7e (routes aligned with binding), L1 permit 8463bd941f, L0R module-load switch c512887606. Commit locally only.
Read first: lad-node-l1/brief.md (module-load plan, PM 12:30), lad-node-l1/call-shape.md, lad-node-l1/tools/* (reuse; copy into lad-node-m1/tools, change warmup to 3 s, runs to 1, output dir). kairos stopped lad-node-l1b before it measured anything (old method).

## Measurement method (실장님 via PM 11-13 15:29 — this replaces every "3 runs / 5 runs / binding every session" line in older briefs and reports)
- Read /root/jobs/fx/cx/v21/s033/common-rules.md (MEASUREMENT METHOD, FIRST OUTPUT, PARITY, build rules). It wins over older briefs.
- Default: previous step + new step(s) only, the 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), 1 run each, all in ONE measure.lock hold. Warmup: Java/.NET 10 s, C++/Node 3 s; active 5 s. The hold should take about 5-10 minutes; do builds, call-shape, aggregation and analysis outside the lock.
- Step loss = 1 − (new ÷ previous) from this hold; ≤ 7% passes. Only cells with loss 5-9% get 3 runs (previous + new, in a second short hold if needed). No 5-run cells, no G5 on 1-run cells.
- Binding: once per language per day. If /root/jobs/fx/cx/v21/s033/binding-daily/<lang>-<YYYY-MM-DD UTC>.json exists, use it as the binding reference; otherwise measure binding 6 cells × 1 run at the start of the same hold and write that file (cell → msg/s + raw path). Report ratio ÷ binding from that file.
- This server (cloudv) can be switched off at any time. After every hold, immediately write the numbers into report.md (append; a partial report is fine) and commit code changes locally before measuring. Do not wait for the end to write results.
- Never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files.

## Do
1. Check lad-node-l1/call-shape.md still matches HEAD (8eb3525b7e changed L0 routes after the table was written); update it as lad-node-m1/call-shape.md. Only the permit may differ at L1; L0R differs from L0 only by the top-level requires.
2. Build (framework/bench/grpc/node/build.sh), dist newer than sources.
3. Binding per the daily rule (node-<date>.json). One hold: L0, L0R, L1 — 6 cells × 1 run each (warmup 3 s). Report three losses per cell: L0R vs L0 (module-load cost), L1 vs L0R (permit cost), L1 vs L0 (the ladder step; ≤ 7% verdict). 5-9% cells → 3 runs of that pair.
4. If L0R vs L0 is above 2% at request-serial 4K: one paired --cpu-prof + process.memoryUsage at end of run for L0 and L0R, frames/GC change with file:line.

## Output
lad-node-m1/report.md (Korean, short tables, numbers cite raw files). Time cap 2 h.
