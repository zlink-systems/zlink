Model gpt-6-luna, effort high: test-only ordering fix with a known cause and file:line; needs careful reading but no product design.

# Task: #4094 — C++ actor late-result test waits forever when the deadline index is not yet published

## Situation
Diagnosis (read it first): /root/jobs/fx/cx/v21/s033/clate/report.md and reproduction-summary.txt; reproducer clate/reproduce.py.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_late_result.cpp `end_local()` (:78-89) reads `next_deadline()` for the timed_out terminal. operation_registry.cpp:428-447 publishes the deadline index asynchronously with `_owner->try_post()` when registration runs off the owner lane, so the test can read an empty deadline; ASSERT_TRUE leaves only the helper, no expire() runs, and the test blocks at :920 on a completion `result()` without a deadline (task.hpp:690-697). Seen 5/317 loaded, 2/1000 whole-binary unloaded.
Worktree /root/worktree/zlink-4094-late, branch fix/4094-cpp-late-result-test = main ce1b779323.

## Continuation (second session)
The first session was stopped by the supervisor (SIGTERM) because its Debug build (-g, ~900 MB per test binary) filled the disk; the build dir was deleted. Its log is clatefix/codex-run1.log. Already done: the before run (clatefix/before-summary.txt: 3 hangs in 44 loaded runs), and the test edit now in the worktree (saved as clatefix/change-partial.diff: lane drain before next_deadline, bounded waits via ready_within). Review that edit, keep or tighten it, then continue from Do step 2 "after".
Build: configure with the release preset (`cmake --preset linux-ninja-release` from framework/languages/cpp, binaryDir overridden to clatefix/build-release with -B), Release, no -g. Build only test_cpp_framework_actor_late_result first; build the remaining unit/contract targets only for the step 3 ctest. Keep clatefix/ under 25 GB (check `du -sh` after each build); if it would exceed, stop and report instead.

## Do
1. Fix the test only: before reading next_deadline(), wait until the owner lane has run the posted index turn — prefer an existing test helper or lane facility (search the test tree for how other tests drain an owner/state lane); else a bounded poll (2 s, like dispatch_request :621-630). Bound the completion waits at :909-920 so a missing terminal fails as an assertion instead of blocking (use an existing bounded wait helper if one exists). No product code change; if you believe product code is wrong, stop and report with file:line.
2. Before/after with clate/reproduce.py adapted into clatefix/ (filter LocalRegistryTerminalsReachPreparedReceiverOnce): main binary loaded until 3 hangs or 500 runs; after: 2000 loaded runs, 0 hangs or failures required; then the whole test binary 300 loaded runs. Same load and pinning as clate.
3. Whole C++ framework unit+contract ctest once (under measure-shared.sh).
4. Format: scripts/format/format.sh --check cpp; git diff --check.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except clate/; write only in clatefix/ and this worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare. Other test runs: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- 리팩토링 점검: list duplicate waits/handoffs in this test with file:line and whether you removed them.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/clatefix/report.md (Korean, plain sentences): change with file:line, before/after counts with log paths, ctest result, format result. Save the diff as clatefix/change.diff.
