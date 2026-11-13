Model gpt-6-luna, effort high: bounded integration fixes on an already-merged bench branch; no product code.

# Task: #4190 integration — finish the cross-language TODOs on the merged driver branch

Worktree /root/worktree/zlink-v21-s033/b4190pr, branch bench/4190-submit-threads = origin/main (with #4230 benchagg judgement) + the four driver branches merged (s4190d .NET, s4190j Java/Kotlin, s4190n Node, s4190c C/C++). Commit on this branch; do not push or open a PR.
Read first: /root/jobs/fx/cx/v21/s033/s4190/common.md (spec summary and every TODO), framework/bench/grpc/README.ko.md §7.2 and §10.2–§10.5, and the four job reports /root/jobs/fx/cx/v21/s033/s4190{d,j,n,c}/report.md.

## Do (each item: file:line before/after in the report)
1. gRPC × request-backpressure is not run (spec: n/a). Every language runner (framework/bench/grpc/*/run_local*.sh and the shared runner_common.sh if it owns the cell list) skips grpc-<lang> × request-backpressure in one place; gRPC send-saturation and request-serial stay. benchagg already renders that cell n/a (#4230) — do not change benchagg judgement.
2. Kotlin supplementary cells (grpc-kotlin, zlink-framework-kotlin) run request-serial @1024 only (#4200): update framework/bench/grpc/java/run_local_kotlin.sh.
3. peak_in_flight = SUM of per-stream maxima in every language (PM decision #4201). Java: remove the shared owner atomics in begin() (framework/bench/grpc/java/.../BenchDrivers.java around :700-703) and sum the stream peaks at freezeTerminal. Check .NET (Program.cs MergeFrom already sums), C, C++ and Node do the same; fix any that do not, and name the field's meaning in the README (ko and en) in one sentence if it is not already stated.
4. Remove nothing else; do not touch product code, benchagg judgement (model.py thresholds / analysis.py), or the measurement-condition scripts (#4162 handles those).
5. Verify: python3 -m unittest discover -s framework/bench/grpc/tools/tests; each touched language's bench unit tests (Java ./gradlew in framework/bench/grpc/java, .NET dotnet test of the bench test project if present, Node npm test in framework/bench/grpc/node, C/C++ ctest as in s4190c report); bash -n for touched shell scripts; a dry listing of each runner's cell plan showing grpc backpressure skipped and Kotlin serial-only (use the runner's own list/dry-run option if it has one; otherwise print the plan from the script logic). No measurements. Smoke runs are NOT needed.
6. C/C++ compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON), Core P=/root/.cache/zlink/core-release/1.24.0/linux-x64; Java/.NET with ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=$P. Remove build outputs at the end.

## Rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Keep raw logs in /root/jobs/fx/cx/v21/s033/s4190i. Report in Korean, short: 결과, 바꾼 것(file:line), 검증(명령·로그), 리팩토링 점검 한 줄, 남은 문제.

## Output: /root/jobs/fx/cx/v21/s033/s4190i/report.md
