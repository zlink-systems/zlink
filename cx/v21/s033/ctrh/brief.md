# Job: C++ target receive-role handoff (S08 §4.4) on the target application path — 0.33.0 #1639 / #3867

Worktree /root/worktree/zlink-3867x (branch perf/3867-cpp-receive-handoff, based on #3957 head 00f9963cc9).
Report: /root/jobs/fx/cx/v21/s033/ctrh/report.md (Korean, plain sentences).

## Decision already made (PM approved, user approved)
Implement the target-only experiment from /root/jobs/fx/cx/v21/s033/crtc/report.md (read sections "현재 실행 경로와 제거 대상",
"명세 조항별 판정", "결정 소유와 교체 범위", "권고와 조건부 구현 순서" first alternative, "가장 작은 판별 실험").
Today on the target, the Framework receive role (src/runtime/mesh/mesh_node_host_service.cpp node_receive_role_t::run/rotate,
dispatch_hosted_ready, mailbox enqueue, notify_application_ready → submit_owner → offload_executor try_submit_internal →
drain_application_owner on another worker) hands every ready owner drain to another application worker. Change only the
execution-resource placement: at the end of a receive rotation, if the existing placement owner (dispatch/offload_executor)
can exclusively assign a currently idle worker to the next receive role, the old resource gives up the receive role and runs
the ready application owner drain itself through the existing drain_application_owner entry. Otherwise keep today's posting path.
Use .NET framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkProcessExecutionPool.cs:389-402 and :578-592
as the reference shape (idle worker removed and marked Receiving, old worker leaves receive then runs application).

## Rules from the spec and the PM (these decide disputes)
- Spec 08-messaging-hot-path.ko.md §4.4 (:300-328) and its "유지하는 계약" table; 01-submit-and-completion.ko.md §11;
  02-handler-turn-and-execution-gate.ko.md §1-3, §13. No spec edits.
- One assignment owner: the existing placement owner atomically takes one idle worker and assigns it exclusively to the next
  receive role (state CAS or equivalent under its existing lock). Posting to an executor or having a waiter is NOT success
  (:309-310). If it cannot take one, no handoff: post as today. No new threads, no raised limits, no wait for the new
  resource to start. At most one receiving resource per socket at any time.
- Decide once per receive rotation, at the rotation boundary, not per record.
- Owner queue, gate, claim, permit, ordering per owner, Yield and reentrancy rules stay with their current owners (1→1).
  The old resource runs application work only after it has fully left the receive role.
- Shutdown/drain order unchanged: stop must still drain receive roles before unbind; a handoff racing stop must not lose or
  duplicate a record or leave a role running.
- Replace in place: one placement path with a fallback branch, not two parallel mechanisms. No new queue, mailbox, pending
  table, flag or counter without removing something. List each decision owner before → after in the report.

## Tests (RED first, then GREEN)
(a) With an idle worker, a rotation with ready application work runs the handler on the resource that received it, and the
next receive runs on a different resource. (b) No idle worker → no handoff, today's path. (c) A handler that blocks does not
stop receiving or other owners' work. (d) Stop during a handoff delivers every admitted record once and leaves no role running.
(e) White-box: concurrent receiving resources per socket ≤ 1; an assigned receive resource is not given application work;
handoff attempt/success counts per rotation are observable in the test (test-only accessor, not a public option).
Existing unit+contract tests must still pass, including #3919's single-delivery tests.

## Build and test
From framework/languages/cpp: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset
linux-ninja-release --parallel 3 -- -k 0`; `ctest --preset linux-ninja-release -L '^(unit|contract)$'`;
`scripts/format/format.sh --check cpp` from the repo root. If the build dir is not configured, configure with the same preset
first (under the same flock -s). Every target must compile before you finish.
Do not run benchmarks; the supervisor measures.

## Limits
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations (no commit, push, branch, stash, reset, rebase, fetch, pull), no spec edits, no GitHub operations.
  Do not run scripts/dev/internal-docs.sh or docs sync scripts. Do not read Core C headers (core/include).
- Write only inside framework/languages/cpp of this worktree and /root/jobs/fx/cx/v21/s033/ctrh/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; never start background jobs with &.
- End the report with "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목" and the decision-owner
  before/after table and the list of changed files.
