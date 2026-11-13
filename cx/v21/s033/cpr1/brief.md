# Job: C++ prepared request — PR 1 of approved alternative B (#3867, 0.33.0 #1639)

Worktree /root/worktree/zlink-3867p (branch perf/3867-cpp-prepared-request, base 00f9963cc9 = #3957 head).
Report: /root/jobs/fx/cx/v21/s033/cpr1/report.md (Korean, plain sentences).

## Approved design (PM approved alternative B; read first)
/root/jobs/fx/cx/v21/s033/ccut/report.md — all of it, especially "Binding 기능과 Framework 메커니즘의 대조", "설계 대안과 최종 호출 구조",
"PR 1. 최종 receiver 기반의 admission·task 저장 표현", "결정 소유 표와 네 runtime 대조", "완료 조건과 리팩토링 점검".
Implement PR 1 only: replace the admission conversion task chain (bool → submit_result → void task, observer coroutine links) and the
result observation objects with views over the one existing per-request storage (the final receiver / request block that #3957 introduced,
with its C1 completion slot). Target on the caller thread: 0 heap allocations and 0 shared_ptr copies per request inside the Framework
result storage/default observation (binding async_result's own shared_ptr and host/port lifetime references are out of scope unless the
existing shutdown/drain order proves them). Caller budget after PR 1: 12.10 µs/request (from 17.48) on request-backpressure 1KiB.

## Hard rules (PM and user)
- User direction: performance is set by Core/bindings; the Framework must only cut its own added overhead, use binding features directly
  and never reinvent what the binding provides. No hotspot trimming outside the design.
- Decision points stay exactly one each: C1 terminal (operation registry take), deadline, admission, typed result delivery. Do not add a
  second completed flag or any new flag/counter/queue without removing the mechanism it replaces. Replace in place: no fast path beside the
  old shared-state path, no request-only fast_task or new public API. Public admission end and physical resource end stay distinct facts.
- User continuations never run on the receive-role stack (08-messaging-hot-path.ko.md:309). Spec: 01-submit-and-completion.ko.md
  (:331–333, :357–365, :392–428, :447–473, :490–519), 08-messaging-hot-path.ko.md, 06-state-ownership-and-lanes.ko.md. No spec edits.
- Blocking result wait, extra observers, ambient context, owner shutdown and gate resume keep working through the same facade; multiple
  public observers use the same owner's extension storage (the 0-heap goal must not cap observer count).

## Tests
RED first where possible. Add counting tests (existing tests/support/allocation_counter.cpp style) proving per request on the ready path:
heap allocations and shared_ptr control-block operations inside Framework result storage = 0 (state precisely what is counted), plus the
existing contract tests: exactly one delivery; exactly one terminal for timeout, cancel, shutdown; never zero deliveries on an exception
path; late physical reply after public timeout handled once; slot not reused before the physical observer ends. Existing unit+contract
suite must pass, including #3919/#3957 tests.

## Build and test
From framework/languages/cpp: configure if needed, then `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake
--build --preset linux-ninja-release --parallel 3 -- -k 0`; `ctest --preset linux-ninja-release -L '^(unit|contract)$'`;
`scripts/format/format.sh --check cpp` from the repo root. All targets must compile. Do not run benchmarks; the supervisor measures.
Another job may hold build locks; wait.

## Limits
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations, no spec edits, no GitHub operations. Do not run docs sync scripts. Do not read Core C headers (core/include).
  Do not modify bindings/.
- Write only inside framework/languages/cpp of this worktree and /root/jobs/fx/cx/v21/s033/cpr1/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; never start background jobs with &.
- End the report with "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목", the decision-owner before/after table,
  the removed mechanisms list and changed files.
