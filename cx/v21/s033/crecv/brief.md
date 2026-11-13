# Job: C++ request receiver — one ownership block, no per-layer re-wrapping (0.33.0 #1639 / #3867)

Model/level: gpt-6.1-sol high. Implementation in worktree /root/worktree/zlink-3867r
(branch perf/3867-cpp-request-receiver = PR #3912 head c8869b63b1). No sub-agents or other models (no Astra).
Do not run scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts.

## Evidence (read first)
- /root/jobs/fx/cx/v21/s033/cladder/report.md — bottom-up ladder. Rung 2 (real raw port + public
  task/admission/completion bridge) drops request-backpressure 1K from 0.70 to 0.38 of binding: caller
  6.18 -> 11.36 µs/msg, plus a completion poller 11.15 µs and completion workers 7.8 µs. Profiled caller:
  allocation 3.125 µs, handoff/synchronization 1.294, task/future internals 0.851.
- /root/jobs/fx/cx/v21/s033/cthin/report.md §2 rows R1, R2, R5, R7 and §3 (what must be kept).

## Goal
Make the request path carry one request-owned receiver instead of re-wrapping it at each layer.
- `include/zlink/framework/contracts/dispatch/request_task.hpp` (`request_task_access_t::receive`,
  `request_task_t::receiver`), `task.hpp` (`task_shared_state_t::on_completed`): co-allocate the admission,
  public-result and physical-lifetime ownership of one request in one block.
- `src/runtime/backend/raw_binding_adapter.hpp:observe_request_completion`,
  `src/runtime/backend/raw_route_port.cpp:request_impl/poll`, `src/runtime/dispatch/provider_task_context.cpp:dispatch_completion`,
  `src/runtime/channels/channel_runtime.cpp` (1770, 1890), `src/runtime/mesh/mesh_node_runtime.cpp:submit_node_request_reply`:
  keep one stable receiver capture and project results directly into it; remove intermediate callable
  owners / per-projection Asio handler allocations.
- Paths are relative to framework/languages/cpp/framework/.

## Must keep (spec)
- framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:61 (admission
  independent of other submissions), :331 (early public completion does not skip physical processing),
  :445 (one pending entry), :490-544 (user callback runs in a new dispatcher turn, never inline on the
  notification/receive stack; no loss at shutdown).
- bindings/doc/spec/async-coroutine-policy.ko.md:160,189 — consume the binding's public `.async()`; no new
  binding callback terminal, no private binding access.
- No new queue, lock, cache, flag or counter authority. Give a decision-owner table with before/after
  location counts; counts must not grow. Do not add a "fast path" API or a second registry.

## Done when
1. All affected C++ targets build: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 cmake --build --preset linux-ninja-release -- -k 0`.
2. `ctest --preset linux-ninja-release -L '^(unit|contract)$'` passes (run under `flock -s` on the
   measure lock). Add or extend tests for: shared task with several observers, early public completion
   with later physical completion, shutdown with pending requests, allocation count per request
   (extend test_cpp_framework_envelope_allocations / send_allocations style).
3. `scripts/format/format.sh --check cpp` passes.
4. Measure with the cladder runner (/root/jobs/fx/cx/v21/s033/cladder/measure-ladder.sh, rung 9 and the
   unlimited Framework cell) and the csubmit runner for request-backpressure 1K and send-saturation 1K,
   condition B, 3 runs, under `flock -o /root/jobs/fx/cx/measure.lock`, one measurement at a time.
   Report msg/s, F/B, per-thread CPU µs/msg before (c8869b63b1) and after, plus allocations per request.
   Before numbers may be reused from cladder/ and csubmit/ if the runner and conditions match.
Keep MemAvailable >= 12 GiB. Stop processes with SIGTERM/SIGINT only.

## Write limits and forbidden
Write only framework/languages/cpp/** in the worktree, plus scratch under /root/jobs/fx/cx/v21/s033/crecv/.
Forbidden: commit, push, branch, stash, reset, rebase, spec or doc edits, GitHub operations.
If a change needs a spec edit, stop and write it up in the report instead.

## Report
/root/jobs/fx/cx/v21/s033/crecv/report.md:
- changed files with one line each on why;
- the decision-owner table;
- tests added;
- before/after measurements with raw paths;
- what remains.
End your final message with:
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"
