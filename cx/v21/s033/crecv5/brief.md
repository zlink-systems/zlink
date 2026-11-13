# Job: PR #3919 — local completion sink without the per-request receiver copy

Model/level: gpt-6-luna max. Worktree /root/worktree/zlink-3867r (branch perf/3867-cpp-request-receiver,
HEAD 5ecae5de8b plus the uncommitted crecv4 changes — keep them; build on them). No sub-agents or other
models (no Astra, no Fable). Do not run scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts.
Do not read Core C headers. Do not fetch the PR. No commit, push, branch, stash, reset, rebase, spec edits
or GitHub ops. Write only under framework/languages/cpp and /root/jobs/fx/cx/v21/s033/crecv5/.
Background: /root/jobs/fx/cx/v21/s033/crecv4/report.md.

## Problem
crecv4 fixed register_local_completion (framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp
~8674-8744) by capturing `receiver = operation.receiver` (a copy) and clearing operation.receiver after the
completion_sink_t is built. operation.receiver is a std::function holding a request_receiver_t
(mesh_node_runtime.cpp:4143), so the copy is one extra heap allocation plus refcount traffic on every local
Actor request. This PR is a performance PR; the hot path must not pay for a failure-only fix.

## Do
Build the sink so the receiver is moved, never copied, and is moved only after every allocation succeeded:
- Turn the lambda into a named functor type (local to the .cpp, anonymous namespace) holding source, result,
  completion, incomplete and a `receiver` member, with the same operator() body (unchanged behavior).
- `auto sink = std::make_shared<functor> (source, result, std::move (completion), std::move (incomplete));`
  (may throw; operation.receiver untouched), then `sink->receiver = std::exchange (operation.receiver, {});`
  (noexcept move), then build the completion_sink_t from the shared owner without a second allocation —
  use the existing `completion_sink_t (std::shared_ptr<TOwner>, TInvoke)` constructor
  (operation_registry.hpp:54, request_task.hpp request_callback_t owner+empty-invoke ctor) or
  request_callback_t::from_shared; check which one adds no allocation and say so in the report.
- Keep the crecv4 catch semantics: construction failure → caller still owns operation.receiver and settles
  it (as now); C1 rejection → the built sink settles. Keep the test fault hook at the point just before the
  receiver move (the existing LocalCompletionSinkConstructionFailureDeliversPhysicalFailureOnce test must
  still pass and still fail if the receiver were moved before the hook).
- Count heap calls: show local Actor request heap calls/request before (5ecae5de8b), with crecv4, and after
  your change, using the existing allocation-counting test infrastructure in
  tests/Zlink.Framework.UnitTests/test_cpp_framework_request_call_allocations.cpp (add a local Actor
  request allocation test if none exists; assert it does not exceed the 5ecae5de8b count).
- Also remove the unused `deliveries` local that crecv4 added in unknown_node_observer_failure
  (test_cpp_framework_request_transport_projection.cpp).
Rule: every request path delivers the physical reply exactly once
(framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:331, :333, :447).
No delivered flag, no engaged-optional check.

## Done when
Full build -k 0, unit+contract ctest, format check, commands exactly as in crecv4/report.md "Verification"
(build under `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh`). MemAvailable >= 12 GiB.
SIGTERM/SIGINT only. Write /root/jobs/fx/cx/v21/s033/crecv5/report.md with the heap-call table and end it with
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목".
