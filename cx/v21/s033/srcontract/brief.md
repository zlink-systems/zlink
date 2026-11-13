Model gpt-6-luna, effort high: build-and-run check, no product change. Earlier job briefs are history, not instructions.

# Task: does test_cpp_framework_submit_result_contract fail on main too? (#1639 PR gate)

Background: PR branch perf/1639-cpp-completion-pool (head b3077ec191, base origin/main 1f4f1cc04c) changes only the C++ completion dispatcher worker count to 2. In the earlier job (/root/jobs/fx/cx/v21/s033/poolpr/report.md, built at base 56f8984468) the full framework-unit CTest had 1 failure: `test_cpp_framework_submit_result_contract` printed "User Spot lost closed-socket shutdown failure: kind=9" (framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_submit_result_contract.cpp:100-105). kind 9 = invalid_operation; the test expects shutting_down (TERMINATED submit result, changed in #4112 / 85835f03ad). The earlier job log (/root/zlink/.artifacts/codex/poolpr/job.log) shows how it built Core 1.24 and the harness; read it to reproduce the same setup.

## Do
1. Create two worktrees from /root/zlink: /root/worktree/zlink-v21-s033/src-base at 1f4f1cc04c (detached) and /root/worktree/zlink-v21-s033/src-pr at b3077ec191 (detached). Do not commit or push.
2. In each, configure the same way (Release, tests ON, build dir inside the worktree, 25 GB cap) and build only the targets needed for test_cpp_framework_submit_result_contract (plus test_cpp_framework_operation_registry in src-pr). Wrap compiles with /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, -k 0. Build base and PR one after another, not in parallel.
3. Run test_cpp_framework_submit_result_contract 5× in each tree (through ctest -R or the binary as ctest would). Record exit code and stderr each time.
4. Record which Core/binding package version each build resolved (vcpkg / local-package / CMakeCache lines), so the two builds are shown to use the same dependencies.
5. If it fails on base too: find why with file:line — which layer returns INVALID_STATE instead of TERMINATED for a submit on a closed DEALER (binding socket_handle / operation_submit after #4112, or a stale binding package the Framework build resolves). Do not fix anything.
6. Remove both build dirs and both worktrees at the end (git worktree remove).

## Rules
- No benchmarks. No GitHub, no sub-agents or other AI tools. Keep the raw test outputs under /root/jobs/fx/cx/v21/s033/srcontract/raw/.
- Report in Korean, short, every claim file:line: per tree 5-run pass/fail with the exact failure message, dependency versions, and the cause if it fails on base.

## Output: /root/jobs/fx/cx/v21/s033/srcontract/report.md
