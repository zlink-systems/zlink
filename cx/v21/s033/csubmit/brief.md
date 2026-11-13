# Job: C++ source submit path — cut submitter CPU per message (#3867, 0.33.0 #1639)

Model/level: gpt-6.1-sol high — interface changes across the public request/send path, registry and
completion owner; earlier luna-level work on this path needed rework.
No sub-agents or other models (no Astra).

## Goal
On the 1K benchmarks the C++ bottleneck is the source submitter thread (cprof: 23.4 µs/msg vs budget
7.56; the multi-worker drain did not move request-backpressure because of this; see
/root/jobs/fx/cx/v21/s033/cdrain/report.md "Observed result"). Breakdown on the submitter
(/root/jobs/fx/cx/v21/s033/zdesign/answer.md): allocation 7.56, bookkeeping 5.06, binding/Core call 5.00 µs.
Done when, on condition B (source 0-7, target 8-15), 3 runs each, main vs branch:
1. source submitter thread µs/msg on request-backpressure 1K drops by at least 30%;
2. request-backpressure 1K and send-saturation 1K Framework/binding medians rise (report both);
3. full C++ framework non-install tests and format check pass.

## Location and write limits
Worktree: /root/worktree/zlink-3867s (branch perf/3867-cpp-submit-path, from origin/main).
Write only under framework/languages/cpp/framework/{src,include}/ and framework/languages/cpp/tests/.
Job files and report: /root/jobs/fx/cx/v21/s033/csubmit/ only (report.md there).

## Forbidden
commit, push, branch, stash, reset, rebase; edits outside the write limits; spec/doc edits; GitHub
issue/board/PR operations. Leave changes uncommitted. C++ builds only via
`/root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 <cmd>`; builds/tests under
`flock -s /root/jobs/fx/cx/measure.lock`, measurements under `flock -o`.

## Basis
- Design to implement: /root/jobs/fx/cx/v21/s033/fdesign/design.md §3.1 Part A (items 1-5) and §4.3
  (files, tests, check). Send path first, then request.
- Spec (framework/doc/framework/common/spec/server/): 01-execution/08-messaging-hot-path.ko.md §3 E1-E4
  (single wire write, registration before submit, same-turn admitted on OK); 01-execution/01-submit-and-
  completion.ko.md §9 (no intermediate async results between layers), §10.1 (single pending entry);
  01-execution/06-state-ownership-and-lanes.ko.md §4 (one protector per state: lane only for `_port`).
- Already merged on this path: #3884 (wire writer), #3888 (submit allocations). Read their diffs first;
  do not redo them.

## Decided design
Follow Part A as written: one completion sink per pending entry replacing the layered result objects
listed there; E1 writes the final frame once (integer deadline formatter producing the same bytes);
remove the `_lifecycle_mutex` reads of `_port` on the lane path; registry node becomes the chain node
(`extract`); inline the `submit_once_t` shared state. Keep wire bytes identical (golden tests must pass
unchanged). Report decision-location counts before/after (Fable §3.6 table) — they must not grow.
Also add the #3888 review follow-ups: assert the completion observer is not called twice, and forbid
moving the completion source after registration (PM review of #3888).

## Verification
Same benchmark runner and pinning as /root/jobs/fx/cx/v21/s033/cdrain/ (reuse its scripts); per-thread
schedstat for the source submitter; allocation census /root/jobs/fx/cx/v21/s033/salloc/census.sh if it
still runs. Logs in the job dir; read first failure + summary only.

## Report
Result per done criterion with numbers, changed files and purpose, decision-location table, verification
commands + log paths + summary lines, the line "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견.
고친 항목, 넘긴 항목", remaining problems. If an item of Part A conflicts with a spec clause, stop that item
and report it with file:line instead of working around it.
