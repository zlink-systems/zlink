# Task: C++ request path — design that takes whole steps off the caller thread (read-only design study)

## Situation (read these first; do not repeat their measurements)
Framework ÷ binding must reach 0.6 (never lower the target). C++ request-backpressure 1KiB: binding 249k req/s, Framework ~52k.
The single application caller (submit) thread is the throughput limit. Budget: 0.6 × 249k = 150k req/s → caller ≤ 6.6 µs/request.
Today the Framework caller is 16.6 µs, of which the binding request call itself is about 3.7 µs (binding caller), so the Framework's
own caller work (~12.9 µs) must fall to about 2.9 µs.
- /root/jobs/fx/cx/v21/s033/cbind/report.md — rate-matched: the binding paced to 52k pays almost the Framework's I/O cost
  (small TCP batches). I/O cost is a rate effect; a faster caller brings it down. Equal-rate Framework-only share: source +42,
  target +21 µs/request, I/O thread only +3.9.
- /root/jobs/fx/cx/v21/s033/ccall/report.md and ccall2/report.md — caller split before/after a rejected PR 1. Trimming objects
  (0 heap, 0 shared_ptr per request) bought only 0.87 µs: the cost is spread over steps, not allocations. Do not propose object
  trimming again.
- /root/jobs/fx/cx/v21/s033/ccut/report.md — earlier option B (rejected in practice by ccall2), option C rejected by spec;
  its binding-duplication table and spec citations are reusable.
- /root/jobs/fx/cx/v21/s033/coreio/report.md, cthin/report.md — background.

## Goal
Design a C++ request path where the caller thread does only what admission needs and every other step leaves the caller
(moved to a lane/thread that processes in batches, or removed). Use binding features first: if the batching lane overlaps the
binding's existing submit queue, batched send, completion delivery or pending-request map, the design must use the binding's
feature as priority 1. If something the binding lacks is needed, list it as a user decision (do not design around it silently).

## Do (worktree /root/worktree/zlink-cdes, main 2f8b8e635a — read only)
1. Per request, list every step the caller thread does today (file:line), with µs from ccall/ccall2, and classify each:
   A) admission-critical (must finish before submit returns per spec), B) can move off the caller, C) can be removed or done once
   per bind/route (not per request), D) already a binding feature (reuse).
   Spec to cite by line: bindings/doc/spec/async-execution-model.ko.md:117-121 (admitted/reply stages; admitted failure → reply fails
   with the same cause; each stage ends exactly once); framework/doc/framework/common/spec/server/01-execution/
   01-submit-and-completion.ko.md (§9–11 around :331-333, :357-365, :392-428, :447-473, :490-519), 08-messaging-hot-path.ko.md §4.4
   (:300-328), 06-state-ownership-and-lanes.ko.md §5. Decision owners stay one each: C1 terminal, deadline, admission, typed delivery.
2. Propose at most two designs. For each: the new caller path step by step with a µs budget per step summing to ≤ 6.6 µs (binding
   call included), and in the same table the cost moved to the receiving lane/thread plus the handoff/wake cost
   (use measured wake/handoff costs from ccall2/cbind/coreio). Show that the source process total at 150k req/s is not worse than
   the binding paced to the same rate plus the Framework's irreducible work (moving cost without lowering the total is rejected).
   State how admission ordering and back-pressure (BACKPRESSURED → WRITABLE re-submit) are preserved when steps are batched.
3. Spec conflicts: list each with line numbers; if any, say what user decision it needs and the µs at stake.
4. Recommend one design and a PR split (each PR with its own caller µs target), and the first PR's file list.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only: no file edits in the worktree, no builds, no measurements, no git operations, no spec edits, no GitHub operations.
  Do not read Core C headers (core/include).
- 리팩토링 점검: write which existing mechanisms the design deletes (list) and confirm no new flag/counter is added without a removal.
- Time box about 2 hours.

## Output: /root/jobs/fx/cx/v21/s033/cdes/report.md (Korean, plain sentences; start with the recommendation and its budget table)
