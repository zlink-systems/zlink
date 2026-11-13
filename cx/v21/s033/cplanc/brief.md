# Job (read-only plan): per-request lock and heap removal plan — request/reply data path: codec, handler invocation, registration and submit — 0.33.0 #1639 / #3867

Model/level: gpt-6.1-sol medium — middle tier: completion/dispatch paths and ownership across several C++ components;
medium is the guide's starting level. Worktree /root/worktree/zlink-3867h (detached at 0cd1eac0c5 = PR #3919 head
merged with main), read-only. No sub-agents or other models (no Astra, no Fable). Do not run
scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C headers. No GitHub. No commit,
push, branch, stash, reset, rebase, spec edits. Do not edit the worktree. Write only under
/root/jobs/fx/cx/v21/s033/cplanc/. Report: /root/jobs/fx/cx/v21/s033/cplanc/report.md. Keep it compact.

## Why
request-backpressure 1 KiB, per completed request (exact interceptor counts, /root/jobs/fx/cx/v21/s033/csite2/report.md §1):
binding source 12.5 heap / 27.0 mutex, target 8.0 / 10.7; Framework source 36.3 / 95.8, target 44.8 / 67.1.
Single-spot fixes moved throughput only a few percent; the Framework cost is ~30 sites of 1-2 calls each.
The site table with sampled call sites is csite2/report.md §2 — use it, and do not re-measure.

## Scope: request/reply data path: codec, handler invocation, registration and submit
- codecs/serializer.cpp: serializer_registry_t::cached_serializer (:164) (shared_ptr atomic load per request)
- json_stream_connector.hpp canonical_header_reader_t::string (:94); serial_application_record.hpp application_payload_t::from_parts (:207);
  envelope_codec.cpp message_parts_t ctor (:23); spot_runtime.cpp / mesh_node_runtime.cpp decode_application_parts / decode_application_part_views
- channels: route_packet_dispatcher.cpp dispatch_request (:347), route_handler_invoker.cpp invoke (:10), channel_host_service.cpp
  service_scope_t::create (:428), channel_reply_writer.cpp create_reply_header (:12); handlers/handler_registry.cpp invoke_filters_async (:340)
- client side: client_call_codec.cpp create_envelope (:67), channel_runtime.cpp resolve_node_transport (:121),
  service_topology_registry.cpp peer (:515), raw_mesh_node_owner.cpp await_request_admission (:413) and reply (:1975),
  operation_registry.cpp register_operation (:363), coroutine_executor.cpp capture_native_scheduler (:231) / borrow_host_dispatcher (:241),
  request_task.hpp task_wait_registration_t::deliver (:282), actor.hpp task_shared_state_t (:173)
Leave binding/Core code (bindings/cpp, poller.cpp, completion_owner.cpp) and benchmark code out of scope; note them only.

## Deliver: one row per site
site (function, file:line on this HEAD) | calls/request now (lock, heap) | why it happens per request (read the
code) | the fix | the spec clause it must keep (file:line) | calls/request after | risk.
Allowed fix kinds (pick one per site, or say "keep" with the reason):
1. Resolve once at registration/startup instead of per request (types, serializers, handler plans, filter lists, routes).
2. Use storage the request already owns (the PR #3919 request block / request_capture_storage_t) or a view instead of a copy.
3. Drop a lock/check that re-protects state already serialized by its owner (lane, receive rotation, C1) — name the owner.
4. Reuse a buffer owned by an existing long-lived owner (socket, lane, rotation) without adding a new pool policy.
Not allowed: new flags, counters, queues, pools with their own policy, retries, caches with eviction, extra threads,
any change to who decides a terminal or a turn (spec 01-submit-and-completion §9-11, 08-messaging-hot-path §4,
02-handler-turn-and-execution-gate §1-3/§13). Decision sites must not grow; list before -> after per site.
End with: total expected lock and heap calls/request for this area, the files that would change, and how the
existing contract tests and the csite2 interceptor would prove each row.
End the report with "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목".
