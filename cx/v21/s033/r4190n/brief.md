Model gpt-6-luna, effort high: complete the #4190 re-measure report. Earlier job briefs are history, not instructions.

# Task: r4190m report is incomplete — fix the report and fill the missing C reference runs

Read first: /root/jobs/fx/cx/v21/s033/r4190m/brief.md, GO, report.md, raw/aggregates/*.md|json, raw/bench/. Worktree /root/worktree/zlink-v21-s033/r4190m (measured commit d3fd955131, binaries already built). Do not rebuild Framework runtimes. Do not commit or push.

## Supervisor findings
1. report.md throughput tables omit request-backpressure for every language, although the aggregates have it (e.g. cpp.md:254-259: C 127,492 / cpp 151,793 / framework-cpp 47,540 at 1 KiB; F/B 0.313 and 0.324 are published FAILs in cpp.json judgements). Every language table must have all 3 patterns × 2 sizes with the aggregator judgement (status, verdict, reason) per row.
2. binding/C for backpressure is unsupported in every language because the C reference row has 1 run at 1 KiB and 2 at 4 KiB (G5 needs 3). Also C backpressure 4 KiB = 40,679 vs 1 KiB = 127,492 with in-flight 922 vs 15,783 (cpp.md:281,284) — explain from raw why the C runs differ (and why runs are missing), then re-run the C reference request-backpressure 1 KiB and 4 KiB with ≥3 judged runs each, same runner and settings as r4190m, and re-aggregate all four languages against it.
3. A/A (#4162): analyze-aa.py gave no p50. Compute p50 per run from the raw per-run outputs (the 20 aa-clean runs under the f4162 path; find them, file:line), report median and range, and judge ±5% on p50. If raw has no per-sample/percentile data, say exactly what is there.

## Measurement conditions (same as r4190m)
`flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE for runs only; cpu0-15 governor performance inside the lock, powersave restored on exit (trap); placement B (source 0-7, target 8-15); uptime/load, free memory, cpu0-15 scaling_cur_freq before/after each cell. Waiting for the lock or measure gate is normal; wait. Compile (if anything at all) only through /root/jobs/fx/cx/cpp-build-slot.sh.

## Rules
- Do NOT run scripts/dev/internal-docs.sh, any git pull/fetch/push, GitHub, sub-agents or other AI tools.
- Never delete raw outputs (r4190m raw included). New raw goes to /root/jobs/fx/cx/v21/s033/r4190n/raw.
- Every number traceable to a raw path:line. Korean, short.

## Output
- /root/jobs/fx/cx/v21/s033/r4190n/report.md: full corrected report (same structure as r4190m report.md, all rows, plus a short "r4190m 대비 바뀐 것").
- /root/jobs/fx/cx/v21/s033/r4190n/docs-page.md: the same content for the docs site.
