Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4328 — (A) fix the test hang via tearDown cleanup, (B) reproduce the ~200 ms delay under the original condition

Worktree /root/worktree/zlink-v21-s033/f4328 (branch fix/4328-disconnect-test-cleanup at current origin/main). Commit locally; do not push.
Read first: /root/jobs/fx/cx/v21/s033/d4328/report.md and its logs (forced-failure-002.*, distribution-summary.txt). PM decisions are below; follow them.

## A. Test hang (product code unchanged)
1. In core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp, move cleanup of every resource a case creates (app poller remove/destroy, monitor close, sockets) into the file's tearDown (track them in file-scope state that tearDown releases and resets), so a failed assertion (Unity longjmp) can no longer leave them for zlink_ctx_term. Remove the now-duplicated tail cleanup. Check EVERY case in this file for cleanup placed after an assertion and cover it the same way; list them in the report.
2. Behavioural fail-before: temporarily force the p95 assertion to fail (limit -1, local only, never committed) and run the binary with a 15 s bound: before the fix it hangs (bounded kill; save gdb stack then SIGTERM, never SIGKILL); after the fix it fails fast with the assertion and exits. Paste both outputs and the exact temporary edit.
3. The test 20× passing, `bash scripts/format/format.sh --check cpp`. Commit: `test(core): disconnect progress test releases poller and monitor in tearDown (Refs #4328)`.

## B. Delay reproduction (diagnosis only)
1. Original condition: full Core ctest `-j4` (Release), repeated with `--repeat until-fail:30` (or a loop of full runs) while nothing else is measuring. Wrap each run in `flock -s /root/jobs/fx/cx/measure.lock` and call `source /root/jobs/fx/cx/measure-gate.sh; measure_gate_pass` before each run. Stop when it fails once or after 30 full runs.
2. Before starting, enable timestamps so the FIRST reproduction is captured: a LOCAL, uncommitted instrumentation patch (keep it as instrumentation.diff in the job dir) that logs steady_clock ns at asio_engine_t::on_read_complete error path (asio_engine.cpp:~941), emit_disconnected/try_claim (asio_engine.cpp:~1915, pipe_transport.cpp:~719), socket_base_t::event_disconnected / enqueue_monitor_event (socket_base_monitor.cpp:~452, ~756), and each asio_poller_t::loop wait entry/exit with its timeout and whether it returned on an event (asio_poller.cpp:~449-455) — only for this test (env-gated in the instrumentation patch is fine since it is never committed). Build the instrumented binaries in a separate build dir; run the repeat with them.
3. If it reproduces: from the timestamps, answer (a) is the gap spent in the 100 ms fallback wait at asio_poller.cpp:455, (b) after enqueue_monitor_event (:756) did the wake reach the waiting poller, or did the poller stay asleep until the timer. A missed wake is the defect; shortening the wait is not a fix. Report the decision site to change; do not change product code.
4. If no reproduction in 30 runs: report run count, durations, and the per-sample distribution from all runs.

## Rules
- Compile only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release, -j3, build dirs under the worktree (build-f4328, build-f4328-inst), 25 GB cap; remove at end.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Raw logs in /root/jobs/fx/cx/v21/s033/f4328/logs; never delete. Hung processes: gdb stack, then SIGTERM, never SIGKILL.
- Do not commit the instrumentation or the forced-failure edit; `git status` must be clean except the A commit at the end.
- Korean, short.

## Output
/root/jobs/fx/cx/v21/s033/f4328/report.md: A (cases checked, diff summary, fail-before/after outputs, 20× result), B (runs, result, timestamp analysis or distribution).
