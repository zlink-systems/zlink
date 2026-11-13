# C++ Framework request submit allocation-site census — issue #3867

## Result

At `request-backpressure`, 1024-byte payloads, the Framework source submitter made **49.298 intercepted heap allocation calls/message**. The binding source submitter made **12.394**. This matches the earlier counter run (49.333 / 12.395). In this stack-traced run, the whole Framework source and target made **67.978 / 52.475 calls/message** (120.453 combined); the binding source and target made **12.982 / 7.778** (20.760 combined).

The tables report sampled call stacks at **1/256 allocation calls**, selected by a per-thread PRNG. The per-thread malloc/calloc/realloc counters are exact for the same active capture window; sampled site shares and requested bytes are expanded to those totals. `realloc` counts once and its requested size contributes to bytes. This is not retained/live memory. The counter intercepts external `malloc/calloc/realloc`, including ordinary `new` calls that reach them; aligned allocation APIs, direct `mmap` and custom pools remain outside the measure.

Site coverage is the share assigned to a resolved Framework/binding frame. If a stack has no such frame, the table labels the benchmark caller where available, or gives a Core/native no-frame row. The C++ binding submitter creates much of its protobuf payload before calling binding APIs, so those allocations are explicitly labeled benchmark caller. Source/target process totals include those rows and Core-only calls.

## Run and confidence

- Preflight passed on `main` at `ee11390257`; the four pre-existing `run_local.sh` pinning edits were preserved. Built Release binaries were used with `SKIP_BUILD=1`; no repo file or binary was changed.
- The only valid benchmark run was `calloc-retry-run1`, one run per implementation, `request-backpressure`, 1024 bytes, 10-second warmup, 20-second active phase. Site capture ran 8 seconds inside active: 335,703 completed replies for Framework and 2,092,356 for binding. Both cells passed count validation with zero completed/received mismatch.
- Every benchmark run was wrapped in `flock -o /root/jobs/fx/cx/measure.lock`; the lock was held by another build for several minutes before the valid run. MemAvailable exceeded 12 GiB at preflight and capture.
- Framework submitter TID 1148948 (`bench_cpp_clien`) contributed 49.298 calls/message; binding submitter TID 1150417 contributed 12.394. Sample counts: 64,795 Framework submitter, 89,582 whole Framework source, 68,804 Framework target, and 101,494 binding submitter stacks.
- Site counts have sampling uncertainty; rescaling reconciles table totals with direct counters. The Framework combined total (120.453) is about 5.1% above the earlier fast counter run (114.633); binding combined total (20.760) is effectively unchanged from 20.695. Treat site shares as current-run evidence, not a claim that stack unwinding has no workload effect.

## Allocation calls and requested bytes per message

### Framework source submitter (critical thread)

Total 49.298 intercepted allocation calls/message; 64,795 sampled stacks; named/mapped call coverage 100.00%. The rows below represent 95.69% of all calls.

| Allocating site (first Framework/binding frame; benchmark fallback labeled) | Calls/message | Requested bytes/message |
|---|---:|---:|
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_task_access_t::receive` | 6.093 | 797.0 |
| `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:route_client_t::submit_request_reply_message_erased` | 5.084 | 1546.3 |
| `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:materialize_binding_part` | 4.010 | 128.4 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_receiver_t copy/constructor` | 3.990 | 301.9 |
| `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:framework_driver_t::run_tasks [benchmark caller]` | 3.023 | 1113.7 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:application_payload_t::from_parts` | 2.997 | 102.5 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:encode_node_request_header` | 2.993 | 34.9 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:task_shared_state_t completion observer` | 2.960 | 407.9 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:route_client_t::request_to_node` | 2.014 | 366.9 |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:raw_mesh_node_owner_t::request_with_header` | 1.990 | 119.2 |
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::request` | 1.962 | 389.6 |
| `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:operation_registry_state_t::register_operation` | 1.942 | 480.8 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:observe_task_terminal` | 1.039 | 16.6 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:classify_node_target_from_topology` | 1.034 | 12.4 |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:envelope_codec_t::encode_header` | 1.033 | 337.9 |
| `bindings/cpp/src/Runtime/Messaging/request_reply.cpp:request_submit_operation_t::async` | 1.028 | 608.5 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:observe_task_completion` | 1.013 | 24.3 |
| `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:public_host_runtime_t::request_to_node` | 1.013 | 12.3 |
| `framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:client_call_codec_t::create_envelope` | 0.981 | 20.6 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:message_parts_t::message_parts_t` | 0.978 | 156.5 |
### Framework whole source process

Total 67.978 intercepted allocation calls/message; 89,582 sampled stacks; named/mapped call coverage 96.80%. The rows below represent 95.70% of all calls.

| Allocating site (first Framework/binding frame; benchmark fallback labeled) | Calls/message | Requested bytes/message |
|---|---:|---:|
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_task_access_t::receive` | 6.077 | 794.9 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_receiver_t copy/constructor` | 6.000 | 1367.3 |
| `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:route_client_t::submit_request_reply_message_erased` | 5.071 | 1542.3 |
| `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:materialize_binding_part` | 3.999 | 128.1 |
| `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:framework_driver_t::run_tasks [benchmark caller]` | 3.015 | 1110.8 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:application_payload_t::from_parts` | 2.989 | 102.2 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:encode_node_request_header` | 2.985 | 34.8 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:task_shared_state_t completion observer` | 2.953 | 407.0 |
| `Core native (no Framework/binding frame in captured stack)` | 2.173 | 6434.8 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:channel_request_call_t::decode` | 2.089 | 1118.0 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:route_client_t::request_to_node` | 2.009 | 366.0 |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:raw_mesh_node_owner_t::request_with_header` | 1.984 | 118.9 |
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::request` | 1.957 | 388.6 |
| `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:public_host_runtime_t::dispatch_ready` | 1.945 | 3808.4 |
| `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:operation_registry_state_t::register_operation` | 1.937 | 479.6 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:next_receive_activity_async` | 1.916 | 849.9 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:observe_task_completion` | 1.318 | 34.1 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:mesh_node_host_service_t::node_receive_role_t::rotate` | 1.299 | 31.2 |
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::poll` | 1.294 | 144.3 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:observe_task_terminal` | 1.037 | 16.6 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:classify_node_target_from_topology` | 1.031 | 12.4 |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:envelope_codec_t::encode_header` | 1.031 | 337.0 |
| `bindings/cpp/src/Runtime/Messaging/request_reply.cpp:request_submit_operation_t::async` | 1.025 | 606.9 |
| `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:public_host_runtime_t::request_to_node` | 1.010 | 12.3 |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:deliver_request_terminal` | 1.008 | 24.2 |
| `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:completion_entry_t::capture` | 0.996 | 159.4 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:task_wait_registration_t::deliver` | 0.996 | 31.9 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:decode_application_part_views` | 0.989 | 31.6 |
| `framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:client_call_codec_t::create_envelope` | 0.978 | 20.5 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:message_parts_t::message_parts_t` | 0.976 | 156.1 |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:request_admission_completion` | 0.968 | 46.4 |
### Framework whole target process

Total 52.475 intercepted allocation calls/message; 68,804 sampled stacks; named/mapped call coverage 95.87%. The rows below represent 96.42% of all calls.

| Allocating site (first Framework/binding frame; benchmark fallback labeled) | Calls/message | Requested bytes/message |
|---|---:|---:|
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::reply` | 4.969 | 285.4 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:mesh_channel_server_builder_t::add_handler` | 4.035 | 2156.1 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:application_payload_t::from_parts` | 4.035 | 263.4 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:decode_application_parts` | 3.911 | 328.7 |
| `framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp:route_handler_invoker_t::invoke` | 2.995 | 3738.7 |
| `Core native (no Framework/binding frame in captured stack)` | 2.170 | 7092.0 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:deserialize_typed_payload` | 2.071 | 1109.9 |
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::receive_if_ready` | 2.045 | 171.5 |
| `framework/languages/cpp/framework/src/runtime/handlers/handler_registry.cpp:handler_registry_t::invoke_filters_async` | 2.036 | 934.4 |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:raw_mesh_node_owner_t::reply` | 2.033 | 61.9 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:mesh_node_host_service_t::dispatch_application` | 2.013 | 193.8 |
| `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:route_packet_dispatcher_t::dispatch_request` | 2.007 | 1388.8 |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:canonical_header_reader_t::string` | 1.977 | 61.3 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:next_receive_activity_async` | 1.962 | 871.1 |
| `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:public_host_runtime_t::dispatch_ready` | 1.944 | 3911.0 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:mesh_node_host_service_t::node_receive_role_t::rotate` | 1.371 | 32.9 |
| `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:make_route_message_context` | 1.025 | 23.6 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:encode_reply_header` | 1.015 | 21.3 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:encode_application_payload` | 1.015 | 1391.7 |
| `framework/languages/cpp/framework/src/runtime/channels/channel_reply_writer.cpp:channel_reply_writer_t::create_reply_header` | 1.014 | 23.3 |
| `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:service_mailbox_t::try_enqueue` | 1.010 | 2974.4 |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:message_parts_t::message_parts_t` | 1.004 | 160.7 |
| `framework/languages/cpp/framework/src/runtime/configuration/services.cpp:service_scope_t::create` | 1.004 | 168.7 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:mesh_record_dispatcher_t::dispatch` | 0.991 | 95.1 |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:envelope_codec_t::encode_header` | 0.944 | 291.8 |
### Binding run source submitter (comparison)

Total 12.394 intercepted allocation calls/message; 101,494 sampled stacks; named/mapped call coverage 100.00%. The rows below represent 99.88% of all calls.

| Allocating site (first Framework/binding frame; benchmark fallback labeled) | Calls/message | Requested bytes/message |
|---|---:|---:|
| `bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:async_result_t::awaiter_t::await_suspend` | 2.645 | 1097.2 |
| `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:zlink_raw_driver_t::make_parts / message_t construction [benchmark caller]` | 2.028 | 1314.4 |
| `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:std::string::_M_mutate [benchmark caller]` | 2.018 | 2071.5 |
| `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:zlink_raw_driver_t::request_submitter [benchmark caller]` | 1.026 | 155.9 |
| `bindings/cpp/src/Runtime/Messaging/request_reply.cpp:request_submit_operation_t::async` | 1.021 | 604.3 |
| `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:protobuf ArenaStringPtr [benchmark caller]` | 1.009 | 32.3 |
| `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:zlink_raw_driver_t::request_submitter poll wait [benchmark caller]` | 1.004 | 128.1 |
| `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:completion_entry_t::capture` | 1.000 | 160.1 |
| `bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp:resume_async_slot` | 0.629 | 25.8 |
### Scope totals

| Scope | Calls/message | Named Framework/binding-frame coverage |
|---|---:|---:|
| Framework source submitter | 49.298 | 100.00% (includes 3.023 calls/message labeled benchmark caller) |
| Framework whole source | 67.978 | 96.80% |
| Framework whole target | 52.475 | 95.87% |
| Binding source submitter | 12.394 | 100.00% including benchmark-caller fallbacks |
| Binding whole source | 12.982 | 95.45% |
| Binding whole target (context only) | 7.778 | 91.42% |

`[Core native ...]` calls have no Framework/binding frame in the captured stack. For Framework target, the representative stack is Core `zlink::asio_engine_t::start_async_read()` → Asio receive operation → IO loop; the 2.170 calls/message row is therefore not attributed to a Framework site. It stays in the table so the process count remains visible. Binding whole target is not a requested table; it is shown only to reconcile the combined process total.

## Framework site object and lifetime interpretation

The counter records allocation size and stack, not C++ dynamic type. The object column below is inferred from the call path in source. “Past submit” refers to ownership after the caller's submit routine returns; target-side rows instead say whether state survives through dispatch. Equivalent binding status describes the binding benchmark path, not necessarily the same public abstraction. The inventory contains Framework sites only; binding-only sites are listed in the binding submitter table.

| Framework site | Allocated object(s) | Past submit? | Binding equivalent |
|---|---|---|---|
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:channel_request_call_t::decode` | Typed reply/request decoded payload object and result wrapper | Reply side: result payload survives to caller; temporary decode state ends at completion | Partial: protobuf payload type exists in binding benchmark, but Framework typed projection does not |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:mesh_channel_server_builder_t::add_handler` | Handler dispatch task/coroutine frame, route context and handler/filter callable storage | Target-side: through handler/result completion | No matching Framework handler/runtime in the binding server run |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:route_client_t::request_to_node` | Typed call/request task state, receiver and routing/coroutine captures | Yes; async result and routing state survive until completion | Partial: binding async_result_t request has reply/admitted stages |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:deserialize_typed_payload` | Typed reply/request decoded payload object and result wrapper | Reply side: result payload survives to caller; temporary decode state ends at completion | Partial: protobuf payload type exists in binding benchmark, but Framework typed projection does not |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_receiver_t copy/constructor` | request_receiver_t copies and its std::function complete/fail/project closures | Yes; stored until admission or reply callback runs | Partial: binding has completion callbacks, but not this receiver/projection object |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_task_access_t::receive` | Admission, public reply and physical reply task shared states plus receiver/observer captures | Yes; distinct stages remain owned through admission/reply terminal | Partial: binding has async_result admission/reply states, not the Framework task projection |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:observe_task_completion` | Observer closure and completion callback storage attached to a task | Yes; until observed terminal, then released | Partial: binding completion callback state is the corresponding mechanism |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:observe_task_terminal` | Observer closure and completion callback storage attached to a task | Yes; until observed terminal, then released | Partial: binding completion callback state is the corresponding mechanism |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:task_shared_state_t completion observer` | task shared state callback storage (std::function and captured scheduler/context) | Yes; until task terminal and continuation delivery | Partial: async_result_state/completion entry serve a related role |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:task_wait_registration_t::deliver` | Await registration, continuation and scheduler delivery record | Yes; through continuation resumption | No same Framework wait registration; binding has awaitable result state |
| `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:materialize_binding_part` | Heap raw_bytes_t ownership wrapper and external native message handle; bytes are moved into retained storage | Yes; native message storage is retained until transport releases it | Yes, at the public message/external-message ownership boundary |
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::poll` | Receive event/result record, message vector and route-poll state | Receive-side only; retained to dispatch boundary, then released/moved | No Framework poll wrapper; binding/Core poller is lower-level |
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::receive_if_ready` | Receive event/result record, message vector and route-poll state | Receive-side only; retained to dispatch boundary, then released/moved | No Framework poll wrapper; binding/Core poller is lower-level |
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::reply` | Reply multipart/vector and native message ownership wrappers | Yes; through send completion/transport ownership | Partial: binding has message parts and one-way send |
| `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::request` | Binding request submission/result wrapper and completion callback captures | Yes; pending reply/admission storage lasts until terminal | Yes; this is the Framework adapter over binding request operations |
| `framework/languages/cpp/framework/src/runtime/channels/channel_reply_writer.cpp:channel_reply_writer_t::create_reply_header` | Header strings, metadata and encoded envelope storage | Usually transient until the native message is built; native bytes persist after submit | No equivalent Framework envelope codec; binding request metadata has separate storage |
| `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:route_client_t::submit_request_reply_message_erased` | Mixed: route/packet strings, metadata/envelope vectors, payload encoder and reply decoder closures, shared target owner | Mixed: encoding temporaries end during submit; decode/target captures live through reply terminal | Partial: binding has operation/serializer inputs but no route client or Framework reply projection |
| `framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp:route_handler_invoker_t::invoke` | Handler dispatch task/coroutine frame, route context and handler/filter callable storage | Target-side: through handler/result completion | No matching Framework handler/runtime in the binding server run |
| `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:make_route_message_context` | Temporary part-view/container or route metadata/context strings | Decode view is transient; context can live through handler dispatch | No equivalent Framework service-wire receive conversion |
| `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:route_packet_dispatcher_t::dispatch_request` | Handler dispatch task/coroutine frame, route context and handler/filter callable storage | Target-side: through handler/result completion | No matching Framework handler/runtime in the binding server run |
| `framework/languages/cpp/framework/src/runtime/configuration/services.cpp:service_scope_t::create` | Handler service-scope owner/control block and scoped provider state | Target-side: through handler completion | No Framework service scope |
| `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:operation_registry_state_t::register_operation` | Pending operation entry, identity/deadline and terminal callback | Yes; through reply, admission failure, timeout or cancellation terminal | Partial: binding completion owner tracks operation completion, not Framework operation registry |
| `framework/languages/cpp/framework/src/runtime/handlers/handler_registry.cpp:handler_registry_t::invoke_filters_async` | Handler dispatch task/coroutine frame, route context and handler/filter callable storage | Target-side: through handler/result completion | No matching Framework handler/runtime in the binding server run |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:mesh_node_host_service_t::dispatch_application` | Handler dispatch task/coroutine frame, route context and handler/filter callable storage | Target-side: through handler/result completion | No matching Framework handler/runtime in the binding server run |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:mesh_node_host_service_t::node_receive_role_t::rotate` | Coroutine frame and receive/dispatch activity result state | Target-side: until activity arrives or coroutine resumes | No matching Framework receive scheduler |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:next_receive_activity_async` | Coroutine frame and receive/dispatch activity result state | Target-side: until activity arrives or coroutine resumes | No matching Framework receive scheduler |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:classify_node_target_from_topology` | Target classification/lookup temporary state (small strings or container growth) | No; lookup ends during submit | No Framework topology resolver in binding path |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:mesh_record_dispatcher_t::dispatch` | Ready/receive record, mailbox queue node and dispatch callback captures | Target-side: through application dispatch/handler completion | No matching Framework target dispatch in binding server run |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:deliver_request_terminal` | Request callback/lambda captures, operation state and coroutine/continuation context | Yes; callback/context lives until admission or physical reply terminal | Partial: binding request operation and completion entry own lower-level terminal state |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:raw_mesh_node_owner_t::reply` | `raw_message_t` multipart vector and encoded reply-header/application-payload buffers passed to `raw_route_port_t::reply` | Encoded temporaries are moved into the send path; native message storage may remain until transport completion | Binding sends message parts, but has no Framework reply codec/owner |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:raw_mesh_node_owner_t::request_with_header` | Request callback/lambda captures, operation state and coroutine/continuation context | Yes; callback/context lives until admission or physical reply terminal | Partial: binding request operation and completion entry own lower-level terminal state |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:request_admission_completion` | Request callback/lambda captures, operation state and coroutine/continuation context | Yes; callback/context lives until admission or physical reply terminal | Partial: binding request operation and completion entry own lower-level terminal state |
| `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:service_mailbox_t::try_enqueue` | Ready/receive record, mailbox queue node and dispatch callback captures | Target-side: through application dispatch/handler completion | No matching Framework target dispatch in binding server run |
| `framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:client_call_codec_t::create_envelope` | Header strings, metadata and encoded envelope storage | Usually transient until the native message is built; native bytes persist after submit | No equivalent Framework envelope codec; binding request metadata has separate storage |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:canonical_header_reader_t::string` | Header strings, metadata and encoded envelope storage | Usually transient until the native message is built; native bytes persist after submit | No equivalent Framework envelope codec; binding request metadata has separate storage |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:envelope_codec_t::encode_header` | Header strings, metadata and encoded envelope storage | Usually transient until the native message is built; native bytes persist after submit | No equivalent Framework envelope codec; binding request metadata has separate storage |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:application_payload_t::from_parts` | Multipart message_parts_t/vector and message_t handles | Container is moved/released during submit; native message buffers can outlive submit | Partial: binding represents parts as vector<message_t> |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:decode_application_part_views` | Temporary part-view/container or route metadata/context strings | Decode view is transient; context can live through handler dispatch | No equivalent Framework service-wire receive conversion |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:decode_application_parts` | Temporary part-view/container or route metadata/context strings | Decode view is transient; context can live through handler dispatch | No equivalent Framework service-wire receive conversion |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:encode_application_payload` | Serialized payload byte vector (about 1.34 KiB requested/message at this site) | Normally no; it is consumed into message parts before submit returns | No same Framework service-wire encoder; binding caller provides message parts |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:encode_node_request_header` | Temporary encoded header byte vector | No; bytes are moved into the message before return | No Framework header format in the binding benchmark |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:encode_reply_header` | Temporary encoded header byte vector | No; bytes are moved into the message before return | No Framework header format in the binding benchmark |
| `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:message_parts_t::message_parts_t` | Multipart message_parts_t/vector and message_t handles | Container is moved/released during submit; native message buffers can outlive submit | Partial: binding represents parts as vector<message_t> |
| `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:public_host_runtime_t::dispatch_ready` | Ready/receive record, mailbox queue node and dispatch callback captures | Target-side: through application dispatch/handler completion | No matching Framework target dispatch in binding server run |
| `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:public_host_runtime_t::request_to_node` | Typed call/request task state, receiver and routing/coroutine captures | Yes; async result and routing state survive until completion | Partial: binding async_result_t request has reply/admitted stages |

Benchmark-caller rows in the call tables are not Framework sites and are omitted from this object inventory. They are `framework_driver_t::run_tasks` in the Framework client and protobuf/string/message construction in `zlink_raw_driver_t::make_parts` in the binding client. The native/Core-only row likewise has no Framework object to inventory.

## Ranked reduction candidates

These are source-based reduction budgets, not measured A/B savings. Calls/message below are the allocation-site evidence or an explicit upper bound; multiple candidates can overlap.

| Rank | Site and concrete change | Calls/message available to remove | Public results/lifetime | Contract |
|---:|---|---:|---|---|
| 1 | `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:encode_application_payload` — serialize directly into the final native message storage so the temporary serialized-payload vector is not allocated and copied into a second storage object. | **Up to 0.965** at this site; 1,341 requested bytes/message. | Preserve encoded bytes and retain native storage through transport ownership; public result and lifetime stay the same. | No change; `05-payload-ownership-and-codec.ko.md` §§1–2, hot-path source stage E1 in `08-messaging-hot-path.ko.md` §3. |
| 2 | `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_task_access_t::receive` plus `request_receiver_t` — co-allocate distinct public/physical task states and move receiver closures once into a request-owned completion block. Keep separate admission and reply completion states. | **At most 1–2** allocation calls/message from the 6.093 `receive` site; receiver copy site is another 2.986 calls/message, but the exact removable fraction is not isolated. | Preserve separate result identities and retain callback state through the existing terminal; no public result or lifetime change. | No change if stage separation remains; Submit/completion §§2, 9 and hot-path E3–E5 in `08-messaging-hot-path.ko.md` §3. |
| 3 | `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:operation_registry_state_t::register_operation` — place the pending entry and its callback storage in one allocation while retaining the same identity, deadline and atomic terminal removal. | **At most 1** call per registered request; current site is 1.942 calls/message. | Preserve pending-entry lifetime through terminal and callback return. | No change if §10 identity, deadline and removal rules remain intact. |
| 4 | `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:materialize_binding_part` — remove the separately heap-allocated `raw_bytes_t` wrapper by transferring payload bytes into the final native message owner; combine with rank 1 so there is one storage allocation, not vector plus owner wrapper. | Site budget **4.010** calls/message; the direct wrapper-call count versus Core message allocations was not separately isolated, so a net call reduction is not established. | Keep bytes alive through transport release; public result/lifetime can remain unchanged. | No change; `05-payload-ownership-and-codec.ko.md` §§2, 4 and 8. |
| 5 | `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_task_t::receive_public` — collapse the independent public reply and physical reply states into one terminal state. | Up to **1 state allocation/message** within the 6.093-call `receive` site; exact allocation-to-state mapping was not tagged. | Changes public completion timing when admission fails before the physical terminal. | **Contract change required:** Submit/completion §§2 and 9, and hot-path E3/E5 in `08-messaging-hot-path.ko.md` §3. |

The current contract requires request admission and physical reply to remain distinct, so rank 5 is not compatible with current results/lifetimes. The top two changes can be pursued without changing public result shape if they preserve the current independent state and callback lifetimes. The exact net reduction for native-buffer work needs a follow-up instrumented A/B run; this census alone cannot assign every call inside `materialize_binding_part` to the wrapper versus Core message object.

## Evidence limits

- This is one instrumented run per implementation. The 1/256 stack sample has many tens of thousands of events per scope, but rare sites and byte estimates have greater variance.
- Allocations above the counted libc APIs and allocations after the 8-second capture window are not site-attributed.
- The binding submitter table includes benchmark caller allocation sites because protobuf payload construction precedes the binding call. Those rows are clearly labeled and are necessary to account for at least 95% of submitter calls.
- Exact object sizes, allocation types, retention durations and post-change savings are not emitted by the allocator counter; those parts are source-based interpretation or stated upper bounds.
- Repository tracked changes: none. Valid workload failures: none. The first attempted cell used the wrong collector path and produced no usable capture; it is excluded. The corrected run passed.
