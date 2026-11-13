Model gpt-6-luna, effort high: change the bench G6 client-saturation rule to one general rule (PM decision 2026-11-11). Earlier job briefs are history, not instructions.

# Task: G6 — saturation at the declared execution-model ceiling is a judged result, not an exclusion

Worktree /root/worktree/zlink-v21-s033/g6rule (branch perf/4190-g6-execution-model-ceiling from origin/main). Commit on it; do not push.
Read first: framework/bench/grpc/README.ko.md §5.1 (G6, ~line 271-290) and §10.3 (~603-620); framework/bench/grpc/tools/benchagg/analysis.py (~158-210) and model.py (~229); /root/jobs/fx/cx/v21/s033/n4190c/report.md (investigation); /root/jobs/fx/cx/v21/s033/r4190m/raw/aggregates/node.json (Node cells blocked by G6).

## Decision (PM, approved)
One rule for every language, no branch on language name:
- Each language driver declares the ceiling of the execution model its spec defines (stream count for C++/.NET/Java/Kotlin per §10.3; Node: one event loop, ELU 1.0).
- If the declared ceiling equals the spec execution-model ceiling for that pattern, a cell saturated at that ceiling is NOT excluded: it is judged and marked "실행 모델 상한 도달" (an aggregator status/flag shown in md and json).
- Only when the driver declares less capacity than the execution model, or declares nothing, does G6 exclude the cell (as today).
- The aggregator reads the declared ceiling and the execution-model ceiling from data (cell metadata: stream count / declared parallelism ceiling; the spec model value must come from the cell's recorded stream count or a single table keyed by pattern, not from a language-name switch). Find where each driver records them (C++ bench_cpp_client.cpp ~928-943, Java BenchDrivers.java ~155-157, .NET Program.cs ~956-970, Node bench-core.js ~8, ~286-291) and say file:line. If a driver does not record what is needed, add the field in the driver's result writer (smallest change).

## Do
1. Spec text in README.ko.md §5.1 (and §10.3 cross-reference if needed): the general rule above, one place. Keep the rest of G6.
2. Aggregator change + unit tests in the existing benchagg test location: (a) Node-like cell saturated at declared 1.0 = model 1 → judged with the mark; (b) a driver declaring less than the model → excluded; (c) no declaration → excluded; (d) C++/Java/.NET cells unchanged.
3. Re-run the aggregator (no new measurement) on /root/jobs/fx/cx/v21/s033/r4190m/raw for all four languages with the new code. Output to /root/jobs/fx/cx/v21/s033/g6rule/aggregates/{cpp,dotnet,java,node}.{md,json}. Show a before/after table of every judgement whose status or verdict changed (expected: only Node; anything else must be explained).
4. Rule count: number of G6 decision sites before → after (should not grow beyond one rule).
5. Run framework/bench/grpc format/lint checks that exist for these files and the benchagg tests.

## Rules
- Do NOT run scripts/dev/internal-docs.sh, any git pull/fetch/push, GitHub, sub-agents or other AI tools. No measurements.
- Never delete raw outputs.
- Korean, short: 바꾼 것(file:line), 규칙 수, 시험, 재집계 전후 표, 남은 문제.

## Output: /root/jobs/fx/cx/v21/s033/g6rule/report.md
