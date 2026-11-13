Model gpt-6.1-sol, effort high: binding socket lifecycle under concurrency in C++ — builders keep raw socket pointers, so submit vs close ordering needs a careful ownership design and a load race test.

# Task: C++ binding finishes submits on a closing/closed socket with TERMINATED, never passing a released handle to Core (#4060, C++ PR)

## Situation
Issue #4060 is the binding contract: bindings/doc/spec/README.ko.md:1851 — reply().submit() after the socket closed gives SubmitError(TERMINATED); a submit on a closing or closed socket must not touch the native handle. :498 — the binding adds no socket-local gate; concurrent independent records follow Core. Core: core/doc/spec/core/socket/README.ko.md :58, :647, :657 — close gives EBUSY while an admitted API runs on the same handle; ESHUTDOWN after close accepted; a released pointer must not be passed (EFAULT/ESTALE).
The Java PR #4073 fixed it with a reservation count taken in the existing lock section that checks closed, released atomically; close returns BUSY while reserved and the caller's close retry decides; one TERMINATED predicate. Design and tests: /root/jobs/fx/cx/v21/s033/bclose/report.md and change.diff (read for the approach and the race test shape; C++ need not copy it line by line).
Known C++ gap (static reading, verify it): bindings/cpp/src/Runtime/Sockets/router.cpp:26, :89, :103 store a raw socket pointer in send/request/reply builders. Runtime/Messaging/operation_state.hpp:116, :125 weak_ptr/lock guard only the runtime object, not native close. Send enters Core at operation_submit.hpp:59, :64; request :165; reply reply_operations.cpp:34. Runtime/Native/socket_handle.hpp:59 valid check and :74 zlink_close have no reservation between them; :76 sets an atomic flag but :81 handle() returns the stored raw pointer. Runtime/Sockets/socket.cpp:86 close runs native close at :90 before shutting down the completion owner at :95.
PM decision: this is a separate C++ binding PR (Refs #4060), landing before #3867. The PUB publish path must be included: check whether publish vs close has the same window and block it in the same mechanism; the PR body states the result.
Worktree: /root/worktree/zlink-4060-cpp-binding, branch fix/4060-cpp-binding-submit-close = latest main f784975639.

## Do
1. Trace submit (send, request, reply) and publish paths and the close path with file:line: where the handle is read, where Core is entered, where it is released, and each window. Also list (report only) other handle uses racing close (receive, options, subscribe).
2. Design the smallest change in the existing lifecycle owner (socket_handle / socket / completion owner) so a submit or publish either enters Core while the handle is guaranteed alive, or ends with TERMINATED without native access. Builders that hold a socket pointer must go through that owner. No socket-local gate: concurrent submits must not serialize; at most one lock entry per submit where main had one, or lock-free with a written ordering argument (memory orders stated). One TERMINATED predicate; give the decision-site count before/after. Explain any new field. Close while reserved returns BUSY (or the binding's existing close-retry behaviour; say which and why).
3. Tests in bindings/cpp/tests (contract or stress, follow the existing layout): same-handle reply x close, send x close, request x close, publish x close races, 1000 repetitions each under stress-ng load (load inside /root/jobs/fx/cx/measure-shared.sh, stop with SIGTERM). Outcomes only success or TERMINATED. Run the same tests on main first and record the failures (counts of EFAULT/ENOENT/INVALID_HANDLE/NOT_FOUND or other). Then the full existing C++ binding test suite.
4. Format: `scripts/format/format.sh --check cpp` if it covers bindings/cpp, else clang-format with the repository config on touched files only; git diff --check.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except bclose/; write only in cbclose/ and this worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or other bindings or framework/.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare. Other test runs: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. No long -j1 full builds. Keep your own output short.
- 리팩토링 점검: list duplicate checks/handoffs you see with file:line and whether your change removed them.
- Time box about 5 hours.

## Output: /root/jobs/fx/cx/v21/s033/cbclose/report.md (Korean, plain sentences): (1) paths and windows with file:line, other racing uses; (2) design, reserve/release sites, lock entries per submit main vs after, decision sites; (3) tests with log paths, main vs after 1000-run outcomes; (4) PUB publish result; (5) duplicate work. Save the full diff (including new files) as cbclose/change.diff.
