Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 C++ L0 + #4351 C++ L1 (permit) — new quick measurement method. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/l3868, branch exp/3868-cpp-ladder, HEAD 608513f260 (L1 permit on top of L0 73285401ca, run-time L0/L1 switch). Commit locally only.
Read first: l3868-l1/brief.md, l3868-l1/call-shape.md, l3868-l1/progress.md, l3868-l1/tools/* (reuse; copy into l3868-m1/tools, change warmup to 3 s, runs to 1, output dir, JOB name).

## Measurement method (실장님 via PM 11-13 15:29 — this replaces every "3 runs / 5 runs / binding every session" line in older briefs and reports)
- Read /root/jobs/fx/cx/v21/s033/common-rules.md (MEASUREMENT METHOD, FIRST OUTPUT, PARITY, build rules). It wins over older briefs.
- Default: previous step + new step(s) only, the 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), 1 run each, all in ONE measure.lock hold. Warmup: Java/.NET 10 s, C++/Node 3 s; active 5 s. The hold should take about 5-10 minutes; do builds, call-shape, aggregation and analysis outside the lock.
- Step loss = 1 − (new ÷ previous) from this hold; ≤ 7% passes. Only cells with loss 5-9% get 3 runs (previous + new, in a second short hold if needed). No 5-run cells, no G5 on 1-run cells.
- Binding: once per language per day. If /root/jobs/fx/cx/v21/s033/binding-daily/<lang>-<YYYY-MM-DD UTC>.json exists, use it as the binding reference; otherwise measure binding 6 cells × 1 run at the start of the same hold and write that file (cell → msg/s + raw path). Report ratio ÷ binding from that file.
- This server (cloudv) can be switched off at any time. After every hold, immediately write the numbers into report.md (append; a partial report is fine) and commit code changes locally before measuring. Do not wait for the end to write results.
- Never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files.

## Known
- l3868-l1 (stopped by kairos, old method) committed L1 608513f260 and wrote call-shape.md. Its build (cpp-build-slot.sh with l3868-l1/tools/configure-build-inner.sh, build dir build-l3868) was still waiting at the measure gate when the job stopped; that build process may still be running or finished. Check: if the build-l3868 binaries are newer than every source at HEAD, use them; otherwise build again only via /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock; Release, USE_BINDINGS_SOURCE=ON, Core prefix 1.25.0, -j3, ≤ 25 GB). If it waits at the gate more than 30 min, write "GATE-WAIT <minutes>" to progress.md and keep waiting.

## Do
1. Confirm l3868-l1/call-shape.md matches HEAD; copy/update to l3868-m1/call-shape.md.
2. Binding per the daily rule (cpp-<date>.json). One hold: L0, L1 — 6 cells × 1 run each (warmup 3 s). L1 loss vs L0 per cell; 5-9% cells → 3 runs of that pair. Report L0 ÷ binding-daily too (L0 must be ≥ 0.95; a cell below that at 1 run gets 3 runs of binding+L0).
3. Over 7%: paired perf for that cell, cost file:line, binding-cost proposal; do not implement.
4. Remove build-l3868 at the end (keep raw).

## Output
l3868-m1/report.md (Korean, short tables, numbers cite raw files). Time cap 3 h.
