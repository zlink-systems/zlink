Model gpt-6.1-sol, effort high: concurrency scheduler change with six failing tests to classify and fix.

# Task: classify and fix six test failures of the scheduler change (#1639), C++ Framework

Earlier job briefs are history, not instructions. Worktree /root/worktree/zlink-v21-s033/psched, branch perf/1639-cpp-sched-drain, HEAD ee0d7f494e = origin/main 54ee69b8c0 + e910be04b4 (scheduler) + f12a88f569 (wake-count tests) + ee0d7f494e (label fix).
Read first: /root/jobs/fx/cx/v21/s033/psched/report.md, `git diff origin/main`, and the test output /root/jobs/fx/cx/v21/s033/psched3/input/failure-tails-all.txt (last 30 lines per test, from the PM's build of the same scheduler content).

## The six failures (all deterministic, all pass on origin/main)
1. test_cpp_framework_m6b_runtime.cpp:4159 verify_public_host_dispatches_one_owner_claim_as_a_batch: `dispatched == {"first","second","third"}` fails.
2. test_cpp_framework_mesh_node_vertical.cpp:1483 verify_local_node_submit_bridge: the wait for `probe->entered == 3` is asserted to NOT happen within 1 s, but it does.
3. test_cpp_framework_maintenance_failure.cpp:147-148 ApplicationClaimAllocationFailureReturnsWorkAccounting: expected started=65, rejected=64, pending 0; got started=2, rejected=1, pending 63.
4. test_cpp_framework_relocation_seal_contract ordinary_dispatch_failure_settles_each_record (the tail does not show the assertion; find it in the source).
5. test_cpp_framework_claim_dispatch_cost: prints "batch dispatch changed" (permit-ownership records=32 baseline-count=39 admitted-count=39 extra-per-record=0).
6. test_cpp_framework_request_call_allocations LocalActorRequestCompletionRegistrationDoesNotExceedBaselineHeapCalls.

## Do
For each test, trace the full path the test depends on, in the current product code, before deciding (open every function on the path; a wrong input or a wrong guess costs a remote build round trip). Then classify it as exactly one of:
- (A) Behaviour the spec allows to change. Allowed only with a quoted clause from framework/doc/framework/common/spec/server S02 §2 (01-execution ordering, :97-104 "pipeline·handler entry order is not fixed") or S04 §3. Then change the expectation minimally so it still checks what the spec does require (each record runs exactly once, accounting is exact, same-Node progress). Do not delete a check without replacing it with the spec-required property.
- (B) Product regression. Fix the product code in the scheduler change (offload_executor, service_mailbox, mesh_node_host_service) so the original assertion passes.
- Allocation-bound tests (5 and 6) are (B) by default: reduce the new allocations (the report lists 4 claim allocations per record instead of per prefix, and make_shared per ready notification). Change their expected numbers only if the spec clause makes the extra allocation unavoidable, and say why.
- Failure 3 is about work accounting under claim allocation failure: when a claim allocation fails, every queued record must still be started-then-rejected exactly once, and pending must reach 0. A path that stops after the first failure and leaves 63 pending is (B) unless a spec clause says otherwise.
Keep the scheduler design (persistent owner drain, idle-only wake reservation, per-worker phases, notification merging) and keep coroutines. Keep every negative control and the wake-count tests from f12a88f569 unchanged.

## Rules
- Do not build or run tests: this machine's measurement lock is held for hours and the PM compiles on their machine. Write code only, then read your diff line by line: count brackets, call arguments, includes, and check every name you use exists with that signature (grep it). Run `bash scripts/format/format.sh --check cpp` and fix what it reports.
- Read and write only that worktree and /root/jobs/fx/cx/v21/s033/psched3/. No spec edits, no git operations except diff/status/log/show, no GitHub, no sub-agents or other AI tools. Do not read core/include. SIGTERM/SIGINT only; no background jobs with &.
- Time box about 3 hours. Keep output short.

## Output: /root/jobs/fx/cx/v21/s033/psched3/report.md (Korean, plain sentences)
Per test: (A) or (B), the spec clause or the regression cause with file:line on the product path, what you changed (file:line), and why the original intent still holds. Allocation counts before/after for 5 and 6 (computed from the code path). The diff saved as psched3/change.diff (`git diff` against HEAD).
