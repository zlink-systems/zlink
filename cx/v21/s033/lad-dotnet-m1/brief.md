Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 .NET L0 send header fix + #4351 L1 recheck + #4352 L2 envelope header — quick measurement method. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-dotnet, branch exp/4350-dotnet-ladder, HEAD b6f58168a6 (L1 permit on top of L0 receive parity a542ee2384). Commit locally only.
Read first: lad-dotnet-l1b/report.md, lad-dotnet-l1b/call-shape.md, lad-dotnet-l1b/tools/* (reuse; copy into lad-dotnet-m1/tools; change warmup to 10 s, runs to 1, output dir), call-shape/dotnet.md, issues-4351-4357.md section #4352, and for L2 the Java reference lad-java-l2b/report.md (production envelope encode path, prepared names, multipart overload).

## Measurement method (실장님 via PM 11-13 15:29 — this replaces every "3 runs / 5 runs / binding every session" line in older briefs and reports)
- Read /root/jobs/fx/cx/v21/s033/common-rules.md (MEASUREMENT METHOD, FIRST OUTPUT, PARITY, build rules). It wins over older briefs.
- Default: previous step + new step(s) only, the 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), 1 run each, all in ONE measure.lock hold. Warmup: Java/.NET 10 s, C++/Node 3 s; active 5 s. The hold should take about 5-10 minutes; do builds, call-shape, aggregation and analysis outside the lock.
- Step loss = 1 − (new ÷ previous) from this hold; ≤ 7% passes. Only cells with loss 5-9% get 3 runs (previous + new, in a second short hold if needed). No 5-run cells, no G5 on 1-run cells.
- Binding: once per language per day. If /root/jobs/fx/cx/v21/s033/binding-daily/<lang>-<YYYY-MM-DD UTC>.json exists, use it as the binding reference; otherwise measure binding 6 cells × 1 run at the start of the same hold and write that file (cell → msg/s + raw path). Report ratio ÷ binding from that file.
- This server (cloudv) can be switched off at any time. After every hold, immediately write the numbers into report.md (append; a partial report is fine) and commit code changes locally before measuring. Do not wait for the end to write results.
- Never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files.
- LOCK TIME LIMIT: one hold ≤ 30 minutes (common-rules). Plan the hold to take about 10 minutes.

## Known
- lad-dotnet-l1b (old method) was stopped by kairos at the PM's request after its long hold. Its runs 1-3 (14:47-15:15 UTC) give L1 vs L0 losses: send 1K +2.2%, send 4K −2.2%, serial 1K −2.8%, serial 4K +6.5% (boundary), backpressure 1K and 4K too noisy to judge. Runs 4-5 (15:18-15:25 UTC) overlapped kairos's backup work (bfs/du/zstd/tar) and must not be used.
- L0 send header differs from binding (call-shape/dotnet.md, Client/Program.cs:385-391): frameworkTarget sends RawEnvelopeHeaders.FrameworkRequest (+formatMarker, about 20 B, kind 1 even for commands) instead of the raw Command header. That is not a restored feature, so L0 ratios so far are 잠정(L0 header 차이).

## Do
1. Fix the L0 send header: the L0 client must send exactly the binding's raw header (same kind, same bytes); only the restored features may differ at each step. Commit. Write lad-dotnet-m1/call-shape.md for L0, L1, L2 vs binding (four paths, file:line both sides).
2. L2: restore ONLY the envelope header on top of L1 using the production .NET envelope encode/decode path (trace file:line as Java did), behind a run-time switch so L0/L1/L2 run from one build. Commit.
3. Build; binaries newer than sources.
4. One hold: binding per the daily rule (dotnet-<date>.json; none exists yet today → measure binding 1 run first), then L0, L1, L2 — 6 cells × 1 run each. Then request-serial 4K L0 + L1 × 3 runs (boundary cell from l1b) in the same hold if the time fits under 30 min, else in a second short hold.
5. Judge L0 ÷ binding-daily (≥ 0.95), L1/L0 and L2/L1 losses (≤ 7%); 5-9% cells → 3 runs of that pair. Over 7%: confirm production call shape first, else paired trace + cost file:line + binding-cost proposal; no product changes.

## Output
lad-dotnet-m1/report.md (Korean, short tables, numbers cite raw files), appended after each hold. Time cap 3 h.
