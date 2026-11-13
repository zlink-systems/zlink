Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4351 Node L1 — confirm the over-7% cells and find the permit cost. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder, HEAD c512887606 (same build as lad-node-m1). Commit locally only.
Read first: lad-node-m1/report.md, lad-node-m1/call-shape.md, lad-node-m1/tools/* (reuse; copy into lad-node-m2/tools, output dir lad-node-m2/raw). Today's binding: binding-daily/node-2026-11-13.json (do not re-measure binding today unless the UTC date changed).

## Measurement method (실장님 via PM 11-13 15:29 — this replaces every "3 runs / 5 runs / binding every session" line in older briefs and reports)
- Read /root/jobs/fx/cx/v21/s033/common-rules.md (MEASUREMENT METHOD, FIRST OUTPUT, PARITY, build rules). It wins over older briefs.
- Default: previous step + new step(s) only, the 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), 1 run each, all in ONE measure.lock hold. Warmup: Java/.NET 10 s, C++/Node 3 s; active 5 s. The hold should take about 5-10 minutes; do builds, call-shape, aggregation and analysis outside the lock.
- Step loss = 1 − (new ÷ previous) from this hold; ≤ 7% passes. Only cells with loss 5-9% get 3 runs (previous + new, in a second short hold if needed). No 5-run cells, no G5 on 1-run cells.
- Binding: once per language per day. If /root/jobs/fx/cx/v21/s033/binding-daily/<lang>-<YYYY-MM-DD UTC>.json exists, use it as the binding reference; otherwise measure binding 6 cells × 1 run at the start of the same hold and write that file (cell → msg/s + raw path). Report ratio ÷ binding from that file.
- This server (cloudv) can be switched off at any time. After every hold, immediately write the numbers into report.md (append; a partial report is fine) and commit code changes locally before measuring. Do not wait for the end to write results.
- Never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files.
- LOCK TIME LIMIT: one hold ≤ 30 minutes (common-rules). This hold should take about 15 minutes.

## Known (lad-node-m1, 1 run each, 15:40-15:50 UTC)
- L1/L0 loss over 7%: send-saturation 4K 17.2% (L0 78,580 → L1 65,073), request-serial 1K 43.8% (7,968 → 4,481), request-serial 4K 13.7% (7,861 → 6,782).
- The serial 1K value does not fit the others: L1/L0R is 44.4% at 1K but only 4.3% at 4K. A single bad run is likely; confirm before profiling.
- L0 ÷ binding at send-saturation 4K = 0.903 (1 run).

## Do (one hold, ≤ 30 min)
1. 3 runs each of L0 and L1 for send-saturation 4K, request-serial 1K and request-serial 4K (interleave L0/L1 per run), and 3 runs of binding + L0 for send-saturation 4K (L0 ≥ 0.95 check). Record each run's start time.
2. For any cell whose 3-run L1/L0 loss is still over 7%: in the same hold if time allows (else a second short hold), one paired --cpu-prof of L0 and L1 for that cell. Name the frames and file:line that grow from L0 to L1 (the permit acquire/release path in the Framework application job queue), and propose how to make the permit cost fit within 7% by reusing what the binding/Core already does. Do not implement product changes.
3. Write report.md right after each hold.

## Output
lad-node-m2/report.md (Korean, short tables, numbers cite raw files). Time cap 2 h.
