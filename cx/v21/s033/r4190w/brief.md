Model gpt-6-luna, effort high: re-measure the #4190 table with 30 s warmup (PM decision 2026-11-11). Earlier job briefs are history, not instructions.

# Task: #4190 four-language table at warmup 30 s

Worktree /root/worktree/zlink-v21-s033/r4190m (measured commit d3fd955131, binaries already built by r4190m; do not rebuild Framework runtimes unless a binary is missing — then build only that, C/C++ through /root/jobs/fx/cx/cpp-build-slot.sh). Do not commit or push.
Read first: /root/jobs/fx/cx/v21/s033/r4190m/brief.md, GO, report.md, raw/*-driver.log (exact commands and env); /root/jobs/fx/cx/v21/s033/r4190n/report.md (corrected report structure, C backpressure re-runs); /root/jobs/fx/cx/v21/s033/jreg/report.md (why: 2 s warmup measured JIT warm-up).

## Do
1. Re-run exactly the r4190m cell set — C++ 48, .NET 48, Java 48, Kotlin 6, Node 48, C reference 18 (C backpressure with ≥3 judged runs per size, as r4190n) — with WARMUP_SECONDS=30 for every language and binding/Framework/gRPC alike. Same placement, runs (3), active duration (5 s), stream settings as r4190m. Confirm from each result file that the recorded warmup is 30 s (file:line for one example per language).
2. Aggregate with main's aggregator (PRs #4270 G6 rule and #4274 warmup-30 check are both merged): /root/worktree/zlink-v21-s033/agg-main/framework/bench/grpc/tools/bench_aggregate.py (read-only worktree at main f649eef0ec), --min-runs 3, into raw/aggregates/. Every row must show warmup 30; if any row is 'unsupported' for warmup, fix the run, not the aggregator.
3. Report: the full table (3 patterns × 2 sizes × 4 languages: gRPC, binding, Framework msg/s; Framework/binding vs 0.60; binding/C for backpressure vs 0.80; aggregator status/verdict/reason per row incl. "실행 모델 상한 도달"), CPU µs/msg source/target per row (TID union), a column "r4190m(2 s) → 30 s" for Framework and binding msg/s, the C++ absolute column (C++ Framework vs the fastest other-language Framework per cell), Kotlin cell, and the A/A from r4190n if done. Mark every number with its raw path.

- PUBSUB latency (PM 2026-11-11): if a latency cell is invalid because catch-up was not reached, show it as "invalid(catch-up 미도달)" and add the observed latency (p50/p99 from samples, in-flight floor) as "판정 외 참고값" if the raw has it; mark any run with late_sends_gt_1ms > 0 separately. Never judge on reference values.

## Measurement conditions
`flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE for runs only (release between language groups is fine but keep the gap short); cpu0-15 governor performance inside the lock, powersave restored on exit (trap); placement B (source 0-7, target 8-15); uptime/load, free memory, cpu0-15 scaling_cur_freq before/after each cell. Waiting for the lock or measure gate is normal; wait.

## Rules
- Do NOT run scripts/dev/internal-docs.sh, any git pull/fetch/push, GitHub, sub-agents or other AI tools.
- Never delete raw outputs (r4190m/r4190n raw included). New raw to /root/jobs/fx/cx/v21/s033/r4190w/raw.
- Korean, short.

## Output
- /root/jobs/fx/cx/v21/s033/r4190w/report.md and docs-page.md (same content, docs-site form).
