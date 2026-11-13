# Job (read-only diagnosis): C++ receive-role handoff (S08 §4.4) on the request completion path — 0.33.0 #1639 / #3867

Model/level: gpt-6.1-sol high — middle tier because this is a completion/concurrency path design comparison; high because it is an architecture choice. Worktree /root/worktree/zlink-3867h (branch perf/3867-cpp-receive-handoff,
HEAD 5ecae5de8b). No sub-agents or other models (no Astra, no Fable). Do not run scripts/dev/internal-docs.sh,
git pull/fetch or docs sync scripts. Do not read Core C headers. Do not fetch GitHub. No commit, push, branch,
stash, reset, rebase, spec edits or GitHub ops. Do not edit the worktree at all (read-only diagnosis). Write only under /root/jobs/fx/cx/v21/s033/chand/
(job-local copies, instrumented builds and scratch sources go there). Report: /root/jobs/fx/cx/v21/s033/chand/report.md.

## Goal
Framework request-backpressure 1 KB is 22.2 µs/message wall vs binding 4.48 µs (F/B 0.209). The 0.6 target
needs about 7.5 µs. The ladder (/root/jobs/fx/cx/v21/s033/cladder/report.md) shows the task/admission/
completion-delivery stage alone takes throughput from 157K to 84K msg/s. The completion-path profile in
/root/jobs/fx/cx/v21/s033/cpoll/report.md ("Diagnosis before implementation") shows the source receive role
spends ~13 µs/reply, of which post_completions ~4.5 and pthread_cond_signal ~4.1 µs: each completion is posted
to an Asio worker and a worker is woken. Batching those posts (cpoll) did not change throughput.

## Rule (no spec change)
framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md §4.4 (:300-326):
the caller continuation (E5) runs in a new execution turn; a new turn need not be another thread. A resource
holding the receive role (which includes driving the binding poller that receives E5 completion notices) may
run that turn itself only after handing the receive role to another resource. Handoff succeeds only when the
existing resource-placement owner exclusively assigns a currently idle resource to the next receive role and
that resource can receive without waiting on the application queue. Posting to an executor or having a waiter
is not success. If assignment cannot be guaranteed, do not hand off: keep the receive role and leave the turn
to the completion dispatcher (today's behavior). Handoff is decided once per receive rotation, not per record.
No new threads, no raised limits, no wait to confirm the new resource started; one receiving resource per socket.
Also read 01-submit-and-completion.ko.md §11 (new-turn conditions) and 02-handler-turn-and-execution-gate.ko.md
§1-3, §13, and keep every row of the §4.4 "유지하는 계약" table. Completion callbacks stay independent
(no drain that depends on one callback returning).

## Deliver (diagnosis only — the supervisor decides the design before any implementation)
1. Decision-owner table for the request completion path today: for each decision (receive role / poller drive,
   completion take, new-turn creation, worker selection and wake, resume of the caller continuation,
   shutdown drain), the spec owner clause file:line, the C++ location file:line, and how many places decide it.
2. Does C++ implement §4.4 handoff today? Answer with file:line. Measure under the benchmark how often a
   handoff is attempted/succeeds per receive rotation (instrument a job-local build copy only).
3. Per-request counts on the current path, from a job-local counting harness, not time: heap allocations,
   thread handoffs (posts and wakeups) and lock acquisitions, split by caller / receive role / worker.
4. Two or three design alternatives that satisfy §4.4 and the PM conditions below, each with: which decision
   owner changes, expected per-request counts, files touched, risks against 01 §11 and 02 §1-3/§13, and how
   the four contract tests (one delivery; one terminal each for timeout/cancel/shutdown; no zero delivery on
   exception) are kept. Recommend one. Mark expected savings as estimates, not measurements.
## Limits
Builds: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset
linux-ninja-release --parallel 3 -- -k 0` from framework/languages/cpp. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; never run background jobs with &. Keep the report short and the
token use moderate: no repeated full-profile runs beyond before/after.
End the report with "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목".

## PM conditions (added before start; these take precedence)
- First, before changing code: instrument and report how many §4.4 handoffs succeed per receive rotation in
  the current build under the benchmark (§4.4 says to record it). If C++ has no handoff today, say so with
  file:line. If attempts mostly fail, the cause is resource assignment, not the spec.
- On the receive resource, without a handoff, only Framework-internal completion work may run: settle the
  result and hand it to the waiter, stopping right before any user continuation (a C++ coroutine resumes into
  user code). No blocking wait, lock wait or application-queue wait there. User continuations run only after a
  successful §4.4 handoff or on a worker, as today.
- Prove budgets by counts per request, not time: heap allocations, thread handoffs (wakeups/posts) and lock
  acquisitions, with counters in a test.
- The changed path must pass the same contract tests: exactly one delivery; exactly one terminal each for
  timeout, cancel and shutdown; never zero deliveries on an exception path.
- Do not keep two paths: the chosen design will replace the existing path in place (implementation is a later job).
