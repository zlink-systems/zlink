Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4328 — diagnose the ~200 ms TCP disconnect mode on the registered-poller path, and the 60 s hang after the assert. NO product changes yet.

Worktree /root/worktree/zlink-v21-s033/d4328 (detached at current origin/main). Do not commit.
Evidence: /root/jobs/fx/cx/v21/s033/p4244/logs/ctest-full.log (search "registered-close"): p50 0 ms, p95 207, max 208; then "Forced closure of 2 sockets" and ctest Timeout 60 s. Test: core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp (case test_tcp_registered_without_waiter_progresses, limit 200 ms at :339).

## Deliver
1. **Reproduction condition**: run the test binary repeatedly (≥200 runs total) idle and under parallel load (e.g. `ctest -j4` of other Core tests alongside, or `stress-ng --cpu 16` if installed). Report the per-sample distribution of the disconnected delay: how many samples are ~0 ms vs ~200 ms, and whether load changes the frequency. Find the exact constant: grep Core (core/src) for 200 ms values (timers, poll timeouts, reconnect/heartbeat/handshake intervals, `100`/`200` ms defaults) that sit on the "poller registered, no waiter" disconnect path, and confirm with timestamps (strace -f -tt -e trace=epoll_wait,poll,futex,close,shutdown on one slow sample, or monitor event timestamps).
2. **Decision owner table**: who decides that the disconnect is observed and the DISCONNECTED event emitted on this path — file:line, the state read, the wake source. Mark where a sample waits for a timer instead of an fd/mailbox wake.
3. **Hang after assert**: why the failing case never ends (which wait blocks, which sockets are left; `gdb -p` stack of the hung process — save it). Is it the test's cleanup (Unity longjmp skipping poller remove/close) or Core teardown?
4. Fix proposal for 1 and 3 in one paragraph each (Core cause vs test cause), with the decision site to change. No code changes.

## Rules
- Compile only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release, -j3, build dir /root/worktree/zlink-v21-s033/d4328/build-d4328, 25 GB cap; remove at end.
- These are timing runs: take /root/jobs/fx/cx/measure.lock SHARED (`flock -s`) — they are tests, not perf measurements — and stop if a perf measurement needs the lock (guard.sh pauses you).
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Raw logs in /root/jobs/fx/cx/v21/s033/d4328/logs; never delete. Hung processes: save gdb stack, then SIGTERM (never SIGKILL).
- Korean, short tables, every number cites its raw file.

## Output
/root/jobs/fx/cx/v21/s033/d4328/report.md with sections 1–4.
