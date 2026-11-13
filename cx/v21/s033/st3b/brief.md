Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions; read /root/jobs/fx/cx/v21/s033/st3/brief.md and st3/report.md for context.

# Task: #1639 step 3 review fixes (C++ record dispatch at registration)

Worktree /root/worktree/zlink-v21-s033/st3, branch perf/1639-cpp-record-dispatch-plan, HEAD 3db0980890. Do not push, fetch or pull, do not open a PR, do not run scripts/dev/internal-docs.sh. Do not touch dispatch/offload_executor.*, receive rotation, wake or worker policy.

## Supervisor review findings (fix both)

1. Route miss lost the error reply and error report.
   framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:70-74 (HEAD) returns a protocol_error when the record's channel is not in the plan. Before 3db0980890 a per-record route_packet_dispatcher_t handled any channel: a request to a channel with no handler got the "No routed request handler is registered" error reply through reply_route_error with the reply token (route_packet_dispatcher.cpp:350-361 at HEAD), and a send got the dispatch error report. The channel comes from the record or from the peer's header, so a peer can name any channel; now the requester waits for its deadline instead of getting the error at once.
   Fix: keep the plan for registered channels; on a miss, go through the same route_packet_dispatcher_t code path (a cold-path dispatcher built for that channel is fine) so the existing no-handler reply/report decides. Do not add a second no-handler rule. Test: a request record to an unregistered channel name gets exactly one error reply with the existing no-handler error kind; a send gets one error report and one terminal. Show it fails at HEAD and passes after.

2. Node send terminal settle moved from "dispatch returned" to "handler completed".
   mesh_node_host_service.cpp:2320-2346 (HEAD). Before, for a node_send record that the Spot runtime did not handle, the shared terminal lived in the direct_terminal std::function, which was not passed to mesh_record_dispatcher_t::dispatch ({} was passed) and was destroyed at the end of the lambda, so complete_stateful_dispatch and release_mailbox_reservation ran right after dispatch() returned. Now the stack terminal is moved into dispatch() and settles when the handler completes. For a suspending handler that delays the mailbox reservation release and stateful completion — a scheduling change the brief excluded, and it touches the same-owner concurrency defect (#1639 same-owner work in progress).
   Fix: keep the old timing with the stack owner (no shared_ptr, no std::function): when Spot did not take it, settle it after dispatch() returns exactly where the old destructor ran, and pass an empty terminal to dispatch() as before. Keep the Spot promotion path (spot_runtime.cpp:15716-15724 local_terminal) unchanged. Test: a suspending Node send handler — the terminal's release callback runs before the handler resumes (fails at HEAD, passes after). If you find spec text that requires settle at handler completion, stop and report it with file:line instead of choosing.

## Do
- Decision-owner table and rule count before/after for the two fixes (no new decision sites).
- Build with /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock; if the measure gate is closed keep waiting, do not quit), Release, tests ON, ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON (never /root/.cache/zlink/zlink-cpp/<ver>), Core release package /root/.cache/zlink/core-release/1.24.0/linux-x64, -j3, -k 0, 25 GB cap.
- New tests: fail-before at 3db0980890 and pass-after logs kept. test_cpp_framework_channel_messaging 50×, full C++ framework unit+contract suite once, bash scripts/format/format.sh --check cpp. Commit locally on top of 3db0980890. Remove the build dir at the end; keep raw logs under /root/jobs/fx/cx/v21/s033/st3b/logs.
- No benchmarks.

## Rules
No GitHub, no sub-agents or other AI tools. Every claim file:line. Report in Korean, short.

## Output: /root/jobs/fx/cx/v21/s033/st3b/report.md
