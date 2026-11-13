Model gpt-6-luna, effort high: small product change + tests. Earlier job briefs are history, not instructions.

# Task: C++ Framework completion dispatcher uses a fixed 2 workers (#1639)

Worktree /root/worktree/zlink-v21-s033/poolpr, branch perf/1639-cpp-completion-pool at origin/main 56f8984468. Do not push, do not open a PR; the supervisor does that.

## Evidence (already measured, do not re-measure)
- /root/jobs/fx/cx/v21/s033/pool/report.md (3 rounds) and /root/jobs/fx/cx/v21/s033/pool2/report.md (5 rounds send-saturation). Supervisor recount (TID union) for 22k: source process 99.4–99.8 µs/msg at base vs 69.0–72.6 at 2 workers; source wakes/msg 2.97–2.99 vs 1.91–2.03; Spot p99 within base range; send-saturation 5-run median 191,311 within base 190,189–194,350.

## Change (one rule, no new option/env/flag)
1. framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp `completion_dispatcher_t`: worker count becomes the constant 2 instead of max(2, available_processor_count()). Keep the comment's reason (a blocked callback leaves another worker for independent turns) and state the S08 §4.4 sentence that bounds it: independence does not promise new application callback execution while all execution resources are tied up in synchronous waits (framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md, §4.4 table row "완료 callback 사이의 독립"). Remove the available_processor_count include from this file only if nothing else in it uses it; the utility itself stays (channel_host_service.cpp uses it).
2. Check every consumer of the worker count: completion_dispatcher_worker_count (completion_turn.hpp:10-12) and the tests that read it (grep). Update expectations that assumed max(2, CPU count) — but do not weaken any assertion about independence; report each test you change with the old and new line.
3. New test (or extend test_cpp_framework_completion_dispatcher_owner.cpp / test_cpp_framework_operation_registry.cpp, whichever owns this): with the real dispatcher (2 workers):
   a. One completion callback blocks (waits on a latch the test controls); a second, independent completion's callback still runs to completion before the latch is released. This is S01 §11 condition (4).
   b. Two completion callbacks block, occupying both workers; a third operation's terminal is still decided and recorded (registry/task terminal visible, e.g. task state is terminal or the completion is accepted), its callback has not run yet, and after releasing one latch the third callback runs. This documents the S08 §4.4 boundary. No sleeps as proof of absence: use bounded waits only for presence, and for "has not run yet" check a flag after the terminal is observed.
   Add a negative control the same way existing tests here do (see how scheduler/mailbox tests register negative variants in framework/languages/cpp/CMakeLists.txt), e.g. a 1-worker mutant that must fail (a), if the existing test pattern supports it without adding production options; if it would need a production flag, skip the negative control and say why.
4. Docs: if framework/doc or the C++ language docs state the pool size as CPU-count based, update them (grep "available_processor\|completion dispatcher\|worker 수"). Spec text stays unchanged.

## Verify
- Build with /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release preset with tests ON, 25 GB disk cap, build dir inside the worktree. All affected targets must compile with -k 0.
- Run the new/changed tests 50× each, plus the full C++ framework unit test suite once (ctest -j4 is fine). Report counts.
- Run `bash scripts/dev/format.sh --check cpp` (or the repo's format script) and fix.
- Commit locally with a message like `perf(cpp-framework): run completion callbacks on a fixed two-worker pool (#1639)`.
- Remove the build dir at the end.

## Rules
- No benchmarks, no measurement lock needed except what cpp-build-slot.sh does. No GitHub, no sub-agents or other AI tools.
- Every claim file:line. Progress to progress.md; report.md once at the end (Korean, short): diff summary, decision-owner count before/after (pool-size decision 1 → 1), rule count before/after, tests changed (old/new lines), test results.

## Output: /root/jobs/fx/cx/v21/s033/poolpr/report.md
