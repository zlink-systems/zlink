# Task: review fixes on commit 317d4037cc and stacking onto #3884 (issue #3867, C++ request submit allocations)

Worktree /root/worktree/zlink-3867a, branch perf/3867-submit-alloc. Read the previous job report /root/jobs/fx/cx/v21/s033/salloc/report.md and `git show HEAD` first. Also read PR #3884's branch perf/3867-wire-writer (`git fetch origin perf/3867-wire-writer`; report /root/jobs/fx/cx/v21/s033/wire/report.md), which changes the same request path.
C = framework/languages/cpp/framework/src/runtime/.

## Fix 1 — remove the node-owned request wire vector
`raw_mesh_node_owner_t::_request_wire` (C/mesh/raw_mesh_node_owner.hpp/.cpp, used in `request_with_header`) is one buffer shared by every request on the node. Correctness then depends on no request starting another request on the same lane before the first has consumed and cleared it: `header(...)`, `register_operation`, `start_admitted_request`/`port->request` and the completion callbacks they may run synchronously would all have to be proven non-reentrant, now and in future. That is a hidden invariant for one allocation per message. Remove the member and the clearing guard; build the outer part list as a local again. After the stacking step below, the local is W's `std::vector<zlink::message_t>`.

## Fix 2 — per-request routing-id copy in raw_route_port_t::request
The new code calls `_request_receive_source (raw_bytes_t (target_routing_id.begin (), target_routing_id.end ()))`, a new heap copy per request when `admission` is empty. Before, it passed the existing vector by reference. Find whether this branch runs on the benchmark request path (node request with empty admission). If it does, or if you cannot rule it out, change the source callback to take `std::span<const std::uint8_t>` (with its setter and every caller), so no copy is made. Report what you found.

## Stacking onto #3884
Rebase or merge this branch onto origin/perf/3867-wire-writer (which already contains current main). Prefer `git rebase origin/perf/3867-wire-writer`; keep the commit history readable (one commit for the allocation cut plus one for these fixes is fine). Conflicts to expect:
- `request_with_header`: W makes the header callback return `zlink::message_t` and the wire a `std::vector<zlink::message_t>`; this branch changes the target to a span and inlines the callback type. Keep both.
- `raw_route_port_t::request`: W adds a `std::vector<zlink::message_t>` overload through a template `request_impl`; this branch changes target to span, parts to rvalue reference and the callback to `raw_request_callback_t`. Keep one implementation for both part types.
- `encode_node_request_header`: W's native encoder supersedes this branch's reserve change on the hot path; keep the vector encoder in the simplest correct form.
Nothing else in either change may be dropped. List every conflict and how you resolved it in the report.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not push. Commit locally.
- No new flags, counters, modes, fallback paths or shared mutable buffers.
- Build C++ only via `/root/jobs/fx/cx/cpp-build-slot.sh <build-dir> --parallel 3` (read its usage). Reuse /root/jobs/fx/cx/v21/s033/salloc's unit build directory if its configuration matches; every affected target must compile with -k 0.
- Run the C++ unit and contract labels via `/root/jobs/fx/cx/measure-shared.sh ctest ... -L 'framework-unit|framework-contract' --parallel 3 --output-on-failure`.
- Run `bash scripts/format/format.sh --check cpp` from the worktree root and fix.
- Measure: rebuild the C++ bench in this worktree (framework/bench/grpc/cpp/build.sh under the build slot). Apply /root/jobs/fx/cx/ratio034/pin-condition-b.patch to the cpp run_local.sh only while measuring; revert before committing. Alternate this build and /root/worktree/zlink-ratio040 (prebuilt main ee11390257) for request-backpressure, request-serial and send-saturation at 1024 bytes, Framework only, 4 rounds, WARMUP_SECONDS=30 DURATION_SECONDS=5, each run in `flock -o /root/jobs/fx/cx/measure.lock`. Also repeat the submitter heap-calls/message census for request-backpressure 1024 with the same tooling as the previous job (/root/jobs/fx/cx/v21/s033/salloc/census.sh, analyze.py).
- Keep MemAvailable >= 12 GiB. Before each compile check MemAvailable; if below 14 GiB wait. Use only one build slot at a time.

## Output: /root/jobs/fx/cx/v21/s033/sfix/report.md (exact path)
What changed and why, the conflict list, the Fix 2 finding, A/B table (each round and medians), census before/after, test and format results, final commit SHAs, anything not finished.
