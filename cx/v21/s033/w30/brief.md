Model gpt-6-luna, effort high: make 30 s warmup the bench rule (PM decision 2026-11-11). Earlier job briefs are history, not instructions.

# Task: bench warmup = 30 s; any other warmup makes a cell unjudgeable

Worktree /root/worktree/zlink-v21-s033/w30 (branch perf/4190-warmup-30s from origin/main). Commit on it; do not push.

## Why (supervisor finding, verified)
Java Framework request-serial 1 KiB measured 4.6-4.8k msg/s with the runner default WARMUP_SECONDS=2 at every commit from 76a4d78506 to main, and 9.0-9.4k with 30 s warmup on the same commit (raw: /root/jobs/fx/cx/v21/s033/jreg/raw/76a4d78506-warmup30 vs 76a4d78506; report /root/jobs/fx/cx/v21/s033/jreg/report.md). Source+target CPU 486 vs 131 µs/msg: 2 s measures JIT warm-up, not steady state.

## Do
1. framework/bench/grpc/README.ko.md: warmup is 30 s for every language and pattern (§8.2 ~line 500 and the variable table ~line 658, and line ~147 if it states a length). State that a cell whose recorded warmup is not 30 s is not judged. One rule, no language branch.
2. framework/bench/grpc/runner_common.sh:49: default WARMUP_SECONDS 30. Find every other place a warmup default lives (C runner bindings/c/... or framework/bench/grpc/c/run_local.sh, per-language run_local*.sh, driver CLI defaults in C++/.NET/Java/Kotlin/Node clients) and make them all 30 or make them take the runner value; list each file:line.
3. Aggregator (framework/bench/grpc/tools/benchagg): read the recorded warmup per cell (trigger.warmup or the results' warmupSeconds — find what each language records, file:line; normalize units) and mark the judgement unsupported with reason "warmup <x>s, spec requires 30s" when any row in the comparison differs from 30. Rows without a recorded warmup → unsupported with a reason. Unit tests for: 30 → judged; 2 → unsupported; missing → unsupported; mixed rows → unsupported.
4. Re-aggregate /root/jobs/fx/cx/v21/s033/r4190m/raw with the new code (no measurement) into /root/jobs/fx/cx/v21/s033/w30/aggregates/ and confirm every judgement there becomes unsupported for warmup 2 s (that is the expected proof the rule works).
5. Run the benchagg unit tests (python3 -m unittest discover -s framework/bench/grpc/tools/tests) and any existing driver tests for files you touch.

## Rules
- Do NOT run scripts/dev/internal-docs.sh, any git pull/fetch/push, GitHub, sub-agents or other AI tools. No measurements, no C++ builds.
- Korean, short: 바꾼 것(file:line), 규칙 수(warmup 판정 위치 before → after), 시험, 재집계 확인, 남은 문제.

## Output: /root/jobs/fx/cx/v21/s033/w30/report.md
