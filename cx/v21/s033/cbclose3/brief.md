Model gpt-6.1-sol, effort medium: hot-path cost trim of a lock-free reservation already reviewed for correctness; needs care with ownership lifetime across threads.

# Task: C++ binding submit/close (#4060) — remove the extra atomics the reservation added to the send hot path

## Situation
Worktree /root/worktree/zlink-4060-cpp-binding (branch fix/4060-cpp-binding-submit-close), uncommitted change from cbclose and cbclose2. Read /root/jobs/fx/cx/v21/s033/cbclose2/report.md first; the supervisor accepted its correctness work (close wait, tests, formatting revert, 27/27, races 0 unexpected). Keep all of that.
Problem: cbclose2 benchmark (cbclose2/benchmark-summary.txt, PAIR-*.json and logs) shows PAIR inproc 1 KiB lower in 6 of 6 adjacent pairs, ratio 0.916-0.971 (geomean 0.9367) at ~1.8-2.0M msg/s, i.e. about +35 ns per message. PUBSUB geomean 0.982, request-serial 0.995. The bench sends with `socket_.send ().message (msg).submit ()` (bindings/cpp/perf/single/common/perf_single_common.hpp:531-534).
Supervisor reading of the cause: main's builder submit used the cached raw handle with no ownership atomics. Now operation_submit.hpp:39-41 does share_socket_handle (operation_state.hpp:128-133): weak_ptr lock of the runtime (inc+dec), a shared_ptr<socket_handle_t> copy (inc+dec), then reserve CAS and release fetch_sub. That is 6 atomic read-modify-writes per message where main had 0 on this step. send_operations.cpp:43 / request_reply.cpp:80,105 also call share_runtime_state with an extra accepting_submits() pre-check before the reservation, and the sync socket_t::send/publish paths copy `_socket` into socket_call_t (socket.cpp:310-422).

## Do
1. Count atomic RMWs (and plain atomic loads) per message on each submit path, main vs current, as a table: builder send/publish submit (blocking and DONTWAIT), async send, request, reply, socket_t::send/publish. Include anything already in main (weak_ptr store at builder construction etc.).
2. Change the design so each path adds at most the reserve CAS and the release fetch_sub over main:
   - socket_call_t borrows a `socket_handle_t &` instead of owning a shared_ptr copy. At each construction site the caller already holds an owner for the whole call (socket_t member, or a locked runtime shared_ptr that owns the completion owner that owns the handle). Write that owner in one short comment where socket_call_t is declared, and make sure the completion owner never resets its handle pointer while a runtime reference exists.
   - Where the path already locks the runtime (it needs it for completions), reuse that one lock for the handle; do not lock twice. Where main did not lock the runtime at all (sync builder send/publish), find the cheapest owner: e.g. keep a strong handle reference in the operation state taken when the builder is created, if that costs no more than main's existing weak_ptr store; otherwise justify the remaining cost with numbers.
   - Remove the share_runtime_state pre-check if the reservation already decides TERMINATED before any argument preparation that matters; one predicate stays.
3. Tests: rerun admission, races 1000 x 4 under load, full binding ctest under load (same commands as cbclose2). Outcomes still only success or TERMINATED.
4. Benchmark again exactly like cbclose2 (same bench-pair.py, 6 adjacent pairs alternating, uptime logged, load < 4, exclusive measure.lock per pair) for PAIR and PUBSUB; request-serial only if the request path changed. Target: PAIR pair-ratio median >= 0.98. If not reached, run `perf stat -e instructions,cycles` and a short `perf record` on main vs PR PAIR and show where the remaining cycles are.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except cbclose/ and cbclose2/; write only in cbclose3/ and this worktree. Reuse cbclose2/main-tree and cbclose2/build-main for main (do not rebuild main unless needed).
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/, other bindings or framework/.
- Keep formatting rule from cbclose2: unchanged lines byte-identical to main; `git diff origin/main --stat -- bindings/cpp` equals the -w variant.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, Release, no -g; keep cbclose3/ under 15 GB. Other test runs: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file (bench logs/JSON, perf output); do not delete them at the end.
- 리팩토링 점검: list remaining duplicate checks/handoffs with file:line; decision-site count stays 1.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/cbclose3/report.md (Korean, plain sentences): (1) atomic count table main / cbclose2 / now per path; (2) ownership argument for borrowed handles with file:line; (3) tests with logs; (4) benchmark raw per-pair numbers, ratios, load lines; (5) per-file +/- for bindings/cpp. Save the full diff including new files as cbclose3/change.diff.
