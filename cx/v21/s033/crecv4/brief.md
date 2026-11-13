# Job: PR #3919 second review — two hand-off paths that deliver the physical reply 0 times

Model/level: gpt-6-luna max. Worktree /root/worktree/zlink-3867r (branch perf/3867-cpp-request-receiver,
HEAD 5ecae5de8b). No sub-agents or other models (no Astra, no Fable). Do not run
scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C headers. Do not fetch the PR.
Background: /root/jobs/fx/cx/v21/s033/crecv3/report.md (what the previous fix did).
Rule: every request path delivers the physical reply exactly once
(framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:331, :333, :447).
No "already delivered" flag, no engaged-optional check.

## Fix 1 — local callback construction (framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:8664-8665)
The lambda capture does `std::exchange (operation.receiver, {})` before the completion_sink_t is built.
Building it can throw (request_task.hpp:167 make_shared). The try block starts only at :8725. After a throw,
actor_client.cpp:857 sees an empty operation.receiver and the owned-task observer (:899) completes only
admission, so the physical reply is delivered 0 times.
Do: keep operation.receiver until the callback is built and clear it only after the transfer; settle a
construction failure and a C1 rejection in the same submission owner.

## Fix 2 — Node admission observer installation (framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:4452-4485)
Sequence: registry closed, so the callback stays with the caller; then observe_task_terminal throws while
building its ambient snapshot (task.hpp:1432, flow_context.cpp:69) after `completion` was moved into the
observer lambda. The moved callback goes with the lambda; the catch finds both `completion` and
`receiver.complete` empty -> 0 deliveries. The existing test fault hook at :4458 fires before the move, so
tests miss it.
Do: keep the unregistered callback reachable from the catch until the observer is installed (for example
move it into the observer only after installation succeeds, or install with a reference that the catch can
still use).

## Tests (RED first)
For each fix add a test that injects the failure at the exact point (a test-only hook compiled out of
production, like the existing ZLINK_FRAMEWORK_TEST_CREATION_PROBE hooks) and asserts: public result is a
failure, physical result is a failure, exactly one physical delivery (test_creation_probe::request_deliveries).
Show each test failing on 5ecae5de8b first (save the log), then passing.
Note: PublicationExceptionDeliversPhysicalFailureOnce, RejectionBeforeRegistrationDeliversOnce and
RejectedLocalRegistrationDeliversPhysicalFailureOnce also pass on the previous head; leave them, but do not
call them regressions.

## Done when
Full build -k 0, unit+contract ctest, format check, commands exactly as in crecv3/report.md "Verification".
No re-measurement needed (cold paths only). MemAvailable >= 12 GiB. SIGTERM/SIGINT only.

## Write limits and forbidden
Write only framework/languages/cpp/** in the worktree plus scratch under /root/jobs/fx/cx/v21/s033/crecv4/.
Forbidden: commit, push, branch, stash, reset, rebase, spec or doc edits, GitHub operations.

## Report
/root/jobs/fx/cx/v21/s033/crecv4/report.md:
- each fix with file:line;
- the RED/GREEN logs;
- the gate results.
End your final message with:
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"
