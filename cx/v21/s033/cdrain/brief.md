# Job: C++ multi-worker drain for gate-less owners (#1639, spec PR #3891)

Worktree: /root/worktree/zlink-1639c (branch perf/1639-cpp-multi-drain, based on the spec branch of PR #3891)
Report: write /root/jobs/fx/cx/v21/s033/cdrain/report.md (absolute path; nowhere else)
Rules: no sub-agents or other models (no Astra). C++ builds only through
`/root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 <build cmd>`. Measurements under
`flock -o /root/jobs/fx/cx/measure.lock`; builds and tests under `flock -s` on the same file.
Commit locally with clear messages; do not push.

## Contract (read first, it is the authority)
framework/doc/framework/common/spec/server/:
- 01-execution/02-handler-turn-and-execution-gate.ko.md §2 (gate-less bullet), §9 (per-worker budget)
- 01-execution/04-application-job-queue-and-backpressure.ko.md §3 step 5 (permit until first handler
  instruction), "준비된 owner 집합" (membership, wait registration, wake coalescing, internal conditions)
- 01-execution/08-messaging-hot-path.ko.md §4.1 I4, §4.3 W1–W5 and the gate-less paragraphs, §7(f)
- 00-foundation/06-framework-api.ko.md §11 scheduler bound
Background on the current C++ drain: /root/jobs/fx/cx/v21/s033/fdesign/design.md §3.3 and §4.1.

## Task
1. In the C++ framework, let several application workers take records of one gate-less Node/ChannelName
   owner. Today `begin_application_drain` (mesh_node_host_service.cpp, dispatch_application /
   drain_application_owner) makes one worker own the owner. Change it so that, for gate-less owners only:
   - a take hands one queue-head record to one worker; the owner stays in the ready set while untaken
     records remain; the same scheduling state decides enqueue, last-take removal and re-registration;
   - wake an additional worker only when untaken records remain and an idle worker exists with no wake
     already scheduled; never notify unconditionally per record; a worker enrolls as a waiter, checks the ready set again, then sleeps;
   - per-worker drain budget, not reset by reselecting the same owner;
   - permits stay counted until immediately before the handler's first instruction (unchanged point).
   Owners with a gate (Spot, Actor, timers, sessions) must keep their current serial behavior.
   Reuse the existing ready set and offload executor; add no new queue, lock or flag unless you remove an
   equivalent one. Report the decision-location table before/after.
2. Tests (framework-unit / framework-contract labels):
   - two records of one gate-less owner run concurrently: hold A in a synchronous handler with a test
     barrier; B must start; negative control: same with a gated Actor owner, B must wait;
   - take order FIFO; each record runs exactly once; last-take vs enqueue race leaves no stranded record;
   - a lone record is processed with no further enqueue (no missed wakeup) — publish around the waiter
     enrollment boundary;
   - per-dispatch scope isolation and exactly-once terminal cleanup with mixed completion order;
   - shutdown drains taken-but-not-started and running records;
   - permit accounting: a taken record paused before its handler is still counted as queued.
3. Measurement (C++ only, condition B pinning source 0-7 target 8-15, 3 runs each, main vs branch):
   request-backpressure 1K and send-saturation 1K Framework/binding ratio; per-thread µs/msg for target
   workers and receive role; context switches/msg.
   Plus the PM's cell: one hot gate-less owner saturating all workers while a Spot/Actor receives a steady
   low-rate request stream; report the Spot/Actor request latency p50/p99 main vs branch.
4. Done criteria: build + all C++ framework tests pass; `format.sh --check cpp` clean; report has the
   numbers tables, decision-location table, test list, and any spec clause you could not meet exactly.
