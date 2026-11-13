Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4351–#4353 Java ladder L1, L2, L3 — new quick measurement method. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java, branch exp/4350-java-ladder, HEAD 23be466e71 (receive parity 6b77f3b04d + L3 service wire 23be466e71 on top of L2 bf83275257). Commit locally only.
Read first: lad-java-par/brief.md and lad-java-par/progress.md (call-shape lines, tools), lad-java-par/tools/*.sh (reuse them; copy into lad-java-m1/tools and change warmup/runs/output dir).

## Measurement method (실장님 via PM 11-13 15:29 — this replaces every "3 runs / 5 runs / binding every session" line in older briefs and reports)
- Read /root/jobs/fx/cx/v21/s033/common-rules.md (MEASUREMENT METHOD, FIRST OUTPUT, PARITY, build rules). It wins over older briefs.
- Default: previous step + new step(s) only, the 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), 1 run each, all in ONE measure.lock hold. Warmup: Java/.NET 10 s, C++/Node 3 s; active 5 s. The hold should take about 5-10 minutes; do builds, call-shape, aggregation and analysis outside the lock.
- Step loss = 1 − (new ÷ previous) from this hold; ≤ 7% passes. Only cells with loss 5-9% get 3 runs (previous + new, in a second short hold if needed). No 5-run cells, no G5 on 1-run cells.
- Binding: once per language per day. If /root/jobs/fx/cx/v21/s033/binding-daily/<lang>-<YYYY-MM-DD UTC>.json exists, use it as the binding reference; otherwise measure binding 6 cells × 1 run at the start of the same hold and write that file (cell → msg/s + raw path). Report ratio ÷ binding from that file.
- This server (cloudv) can be switched off at any time. After every hold, immediately write the numbers into report.md (append; a partial report is fine) and commit code changes locally before measuring. Do not wait for the end to write results.
- Never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files.

## Known
- lad-java-par measured fresh L0 after the parity fix: binding (zlink-java) and L0 (zlink-framework-java), 6 cells × 3 runs, raw under lad-java-par/raw/fresh/L0/run{1,2,3}. kairos stopped that job before L1–L3 (old method).

## Do
1. Write binding-daily/java-2026-11-13.json from the lad-java-par fresh L0 binding medians (3 runs, cite raw paths) — that is today's Java binding; do not measure binding again today. If the UTC date is already 2026-11-14 when you measure, measure binding 1 run in the hold and write java-2026-11-14.json instead.
2. call-shape.md for L1, L2, L3 vs binding (reuse lad-java-par/progress.md excerpts; only restored features may differ: L1 permit, L2 envelope header, L3 service wire framing). Fix the bench before measuring if anything else differs.
3. Fresh build (build && JARs newer than sources).
4. One hold: L0, L1, L2, L3 — 6 cells × 1 run each (warmup 10 s, active 5 s). Step losses L1/L0, L2/L1, L3/L2 per cell; 5-9% cells → 3 runs of that pair.
5. Over 7%: first confirm the bench uses the production call shape (fix and re-measure if not); otherwise one paired JFR for that cell, cost with file:line, binding-cost proposal. Do not implement product changes.

## Output
lad-java-m1/report.md (Korean, short tables, numbers cite raw files): per step table (cell, previous, new, loss, verdict, ratio ÷ binding-daily). Time cap 2 h.
