# Task: cut heap allocations on the C++ Framework request submit path (issue #3867)

Evidence (read first):
- /root/jobs/fx/cx/v21/s033/calloc/report.md — allocation-site census. request-backpressure 1024: the source submitter thread makes 49.3 heap calls per message (binding 12.4). Its table "Framework source submitter (critical thread)" lists every site with calls and bytes per message.
- /root/jobs/fx/cx/v21/s033/cprof/report.md — stage profile. Allocation is 7.56 of 23.4 µs/message on the submitter, the largest stage, including 2.6 µs of allocator lock waits.

## Goal
Bring the submitter's heap calls per message from 49.3 down to 30 or fewer, with no contract change: every public result, completion identity, ordering and lifetime stays exactly as it is. Change representation only:
- move instead of copy (closures, receivers, strings, vectors);
- put objects that always live and die together in one allocation;
- use inline or fixed-capacity storage for small, bounded data (the census shows sites that allocate 3 times for 35 bytes);
- reuse per-node storage where its lifetime is already owned by one place.

Sites in scope (from the census; C = framework/languages/cpp/framework/src/runtime/, P = framework/languages/cpp/framework/include/zlink/framework/contracts/):
- P/dispatch/request_task.hpp: request_task_access_t::receive (6.1), request_receiver_t copy/constructor (4.0)
- C/channels/channel_runtime.cpp: route_client_t::submit_request_reply_message_erased (5.1)
- C/protocol/service_wire_codec.cpp: encode_node_request_header (3.0) — this function only
- P/dispatch/task.hpp: task_shared_state_t completion observer (3.0), observe_task_terminal (1.0), observe_task_completion (1.0)
- P/channels/channel.hpp: route_client_t::request_to_node (2.0)
- C/mesh/raw_mesh_node_owner.cpp: request_with_header (2.0)
- C/backend/raw_route_port.cpp: raw_route_port_t::request (2.0)
- C/foundation/operation_registry.cpp: register_operation (1.9)
- C/mesh/mesh_node_runtime.cpp: classify_node_target_from_topology (1.0)
- C/messaging/envelope_codec.cpp: encode_header (1.0); C/messaging/client_call_codec.cpp: create_envelope (1.0)
- C/stateful/public_host_runtime.cpp: request_to_node (1.0)

Out of scope: another job is changing service-wire payload encoding and native message materialization. Do not touch encode_application_payload, application_payload_t, message_parts_t, or C/backend/raw_binding_adapter.hpp. The separate public-reply and physical-reply states must stay separate (contract: spec/server/01-execution submit and completion §2, §9).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Worktree /root/worktree/zlink-3867a, branch perf/3867-submit-alloc (from origin/main 3fb411b4ab). Commit there; do not push.
- No new flags, counters, modes, pools with their own policy, or fallback paths. Report rule/decision count before and after. A custom allocator or replacing malloc is not this task.
- Build C++ only via `/root/jobs/fx/cx/cpp-build-slot.sh <build-dir> --parallel 3` (read its usage) under `flock -s /root/jobs/fx/cx/measure.lock`. Every affected target must compile with -k 0 before you call the work done.
- Run the C++ unit and contract labels via `/root/jobs/fx/cx/measure-shared.sh` (read its usage); all must pass. Run `bash scripts/format/format.sh --check cpp` from the worktree root and fix.
- Measure per site: rerun the census counter (/root/jobs/fx/cx/v21/s033/calloc — reuse its scripts) on your build and on /root/worktree/zlink-ratio040 (already built, main ee11390257). Then A/B throughput: build the C++ bench in your worktree (framework/bench/grpc/cpp/build.sh under the build slot), apply /root/jobs/fx/cx/ratio034/pin-condition-b.patch to cpp/run_local.sh for measuring only and revert it before committing. Alternate your build and zlink-ratio040, Framework only, request-backpressure 1024 and request-serial 1024 and send-saturation 1024, 4 rounds, WARMUP_SECONDS=30 DURATION_SECONDS=5. Every bench run in `flock -o /root/jobs/fx/cx/measure.lock`.
- Keep MemAvailable >= 12 GiB.

## Output: /root/jobs/fx/cx/v21/s033/salloc/report.md (exact path)
Per site: change made, calls/message before → after (measured). Total submitter and whole-process calls/message before → after. The A/B table with every round and medians. Tests and format results. Commit SHA. Anything not finished.
