import json, pathlib
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/calloc')
X=json.load(open(J/'analysis.json'))

def site_label(s):
    # Each label names the first Framework/binding frame, or explicitly shows a fallback.
    rows=[
      ('[no Framework/binding frame]', 'Core native (no Framework/binding frame in captured stack)', 'unattributed native frame'),
      ('bench_cpp_client.cpp:', 'framework/bench/grpc/cpp/client/bench_cpp_client.cpp', 'benchmark caller'),
      ('bench_cpp_framework_server.cpp:', 'framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp', 'benchmark handler template'),
      ('bench_cpp_zlink_server.cpp:', 'framework/bench/grpc/cpp/server/bench_cpp_zlink_server.cpp', 'benchmark caller'),
      ('request_task_access_t::receive<', 'framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp', 'request_task_access_t::receive'),
      ('route_client_t::submit_request_reply_message_erased', 'framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp', 'route_client_t::submit_request_reply_message_erased'),
      ('materialize_binding_part', 'framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp', 'materialize_binding_part'),
      ('application_payload_t::from_parts', 'framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp', 'application_payload_t::from_parts'),
      ('encode_node_request_header', 'framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp', 'encode_node_request_header'),
      ('request_receiver_t<', 'framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp', 'request_receiver_t copy/constructor'),
      ('route_client_t::request_to_node', 'framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp', 'route_client_t::request_to_node'),
      ('task_shared_state_t<', 'framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp', 'task_shared_state_t completion observer'),
      ('raw_mesh_node_owner_t::request_with_header', 'framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp', 'raw_mesh_node_owner_t::request_with_header'),
      ('raw_route_port_t::request', 'framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp', 'raw_route_port_t::request'),
      ('operation_registry_state_t::register_operation', 'framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp', 'operation_registry_state_t::register_operation'),
      ('observe_task_terminal', 'framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp', 'observe_task_terminal'),
      ('classify_node_target_from_topology', 'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp', 'classify_node_target_from_topology'),
      ('envelope_codec_t::encode_header', 'framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp', 'envelope_codec_t::encode_header'),
      ('request_submit_operation_t::async', 'bindings/cpp/src/Runtime/Messaging/request_reply.cpp', 'request_submit_operation_t::async'),
      ('observe_task_completion', 'framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp', 'observe_task_completion'),
      ('public_host_runtime_t::request_to_node', 'framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp', 'public_host_runtime_t::request_to_node'),
      ('submit_node_request_reply', 'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp', 'mesh_node_runtime_t::submit_node_request_reply'),
      ('client_call_codec_t::create_envelope', 'framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp', 'client_call_codec_t::create_envelope'),
      ('message_parts_t::message_parts_t', 'framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp', 'message_parts_t::message_parts_t'),
      ('deliver_request_terminal', 'framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp', 'deliver_request_terminal'),
      ('request_admission_completion', 'framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp', 'request_admission_completion'),
      ('encode_application_payload', 'framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp', 'encode_application_payload'),
      ('channel_request_call_t::decode', 'framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp', 'channel_request_call_t::decode'),
      ('public_host_runtime_t::dispatch_ready', 'framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp', 'public_host_runtime_t::dispatch_ready'),
      ('next_receive_activity_async', 'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp', 'next_receive_activity_async'),
      ('raw_route_port_t::poll', 'framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp', 'raw_route_port_t::poll'),
      ('node_receive_role_t::rotate', 'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp', 'mesh_node_host_service_t::node_receive_role_t::rotate'),
      ('completion_entry_t::capture', 'bindings/cpp/src/Runtime/Messaging/completion_owner.cpp', 'completion_entry_t::capture'),
      ('task_wait_registration_t::deliver', 'framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp', 'task_wait_registration_t::deliver'),
      ('decode_application_part_views', 'framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp', 'decode_application_part_views'),
      ('canonical_header_reader_t::string', 'framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp', 'canonical_header_reader_t::string'),
      ('raw_route_port_t::reply', 'framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp', 'raw_route_port_t::reply'),
      ('decode_application_parts', 'framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp', 'decode_application_parts'),
      ('deserialize_typed_payload', 'framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp', 'deserialize_typed_payload'),
      ('receive_if_ready', 'framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp', 'raw_route_port_t::receive_if_ready'),
      ('invoke_filters_async', 'framework/languages/cpp/framework/src/runtime/handlers/handler_registry.cpp', 'handler_registry_t::invoke_filters_async'),
      ('raw_mesh_node_owner_t::reply', 'framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp', 'raw_mesh_node_owner_t::reply'),
      ('mesh_node_host_service_t::dispatch_application', 'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp', 'mesh_node_host_service_t::dispatch_application'),
      ('route_packet_dispatcher_t::dispatch_request', 'framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp', 'route_packet_dispatcher_t::dispatch_request'),
      ('mesh_channel_server_builder_t::add_handler', 'framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp', 'mesh_channel_server_builder_t::add_handler'),
      ('route_handler_invoker_t::invoke', 'framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp', 'route_handler_invoker_t::invoke'),
      ('make_route_message_context', 'framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp', 'make_route_message_context'),
      ('encode_reply_header', 'framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp', 'encode_reply_header'),
      ('create_reply_header', 'framework/languages/cpp/framework/src/runtime/channels/channel_reply_writer.cpp', 'channel_reply_writer_t::create_reply_header'),
      ('service_mailbox_t::try_enqueue', 'framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp', 'service_mailbox_t::try_enqueue'),
      ('service_scope_t::create', 'framework/languages/cpp/framework/src/runtime/configuration/services.cpp', 'service_scope_t::create'),
      ('mesh_record_dispatcher_t::dispatch', 'framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp', 'mesh_record_dispatcher_t::dispatch'),
      ('next_dispatch_activity_async', 'framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp', 'public_host_runtime_t::next_dispatch_activity_async'),
      ('async_result_t<', 'bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp', 'async_result_t::awaiter_t::await_suspend'),
      ('resume_async_slot', 'bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp', 'resume_async_slot'),
      ('basic_string', 'framework/bench/grpc/cpp/client/bench_cpp_client.cpp', 'std::string::_M_mutate [benchmark payload construction]'),
      ('completion_owner_t::drain_impl', 'bindings/cpp/src/Runtime/Messaging/completion_owner.cpp', 'completion_owner_t::drain_impl'),
    ]
    for pat,file,fn in rows:
        if pat in s:
            if file.startswith('Core native'): return file
            if pat.endswith('.cpp:'):
                raw=s.split(':',1)[1].split(' [benchmark caller]')[0]
                if 'framework_driver_t::run_tasks' in raw: fn='framework_driver_t::run_tasks [benchmark caller]'
                elif 'zlink_raw_driver_t::request_submitter' in raw: fn='zlink_raw_driver_t::request_submitter [benchmark caller]'
                elif 'zlink::message_t::message_t' in raw: fn='zlink_raw_driver_t::make_parts / message_t construction [benchmark caller]'
                elif 'basic_string' in raw: fn='std::string::_M_mutate [benchmark caller]'
                elif 'ArenaStringPtr' in raw: fn='protobuf ArenaStringPtr [benchmark caller]'
                elif 'poller_t::impl::wait' in raw: fn='zlink_raw_driver_t::request_submitter poll wait [benchmark caller]'
                elif 'async_result_t' in raw: fn='async_result_t::awaiter_t::await_suspend'
                elif 'deque<' in raw: fn='ready_queue_t::_M_push_back_aux [benchmark caller]'
                else: fn=raw[:105]+' [benchmark caller]'
            return f'{file}:{fn}'
    return 'UNMAPPED: '+s[:130]

def calls_table(impl,scope,title):
    p=X[impl][scope];total=p['calls_per_message'];acc=0;out=[]
    grouped={}
    for r in p['rows']:
        label=site_label(r['site']); row=grouped.setdefault(label,[0.0,0.0])
        row[0]+=r['calls_per_msg'];row[1]+=r['bytes_per_msg']
    for label,(calls,bytes_) in sorted(grouped.items(),key=lambda item:item[1][0],reverse=True):
        out.append(f"| `{label}` | {calls:.3f} | {bytes_:.1f} |")
        acc+=calls
        if acc>=total*0.951:break
    return f"### {title}\n\nTotal {total:.3f} intercepted allocation calls/message; {p['samples']:,} sampled stacks; named/mapped call coverage {p['coverage']*100:.2f}%. The rows below represent {acc/total*100:.2f}% of all calls.\n\n| Allocating site (first Framework/binding frame; benchmark fallback labeled) | Calls/message | Requested bytes/message |\n|---|---:|---:|\n"+'\n'.join(out)+'\n'

# Build the union of Framework/binding sites from the requested four tables and F process tables.
scopes=[('framework','submitter'),('framework','source'),('framework','target'),('binding','submitter')]
site_records={}
for impl,scope in scopes:
    p=X[impl][scope];acc=0
    for r in p['rows']:
        label=site_label(r['site'])
        site_records.setdefault(label,{'site':r['site'],'impl':impl,'scopes':set()})['scopes'].add((impl,scope))
        acc+=r['calls_per_msg']
        if acc>=p['calls_per_message']*.951:break

# Descriptions inferred from inspected call paths; type tags are not present in malloc records.
def object_info(label):
    s=label
    if 'Core native' in s:return None
    if 'benchmark' in s:return None
    if 'request_task_access_t::receive' in s:return ('Admission, public reply and physical reply task shared states plus receiver/observer captures', 'Yes; distinct stages remain owned through admission/reply terminal', 'Partial: binding has async_result admission/reply states, not the Framework task projection')
    if 'request_receiver_t' in s:return ('request_receiver_t copies and its std::function complete/fail/project closures', 'Yes; stored until admission or reply callback runs', 'Partial: binding has completion callbacks, but not this receiver/projection object')
    if 'task_shared_state_t' in s:return ('task shared state callback storage (std::function and captured scheduler/context)', 'Yes; until task terminal and continuation delivery', 'Partial: async_result_state/completion entry serve a related role')
    if 'observe_task_terminal' in s or 'observe_task_completion' in s:return ('Observer closure and completion callback storage attached to a task', 'Yes; until observed terminal, then released', 'Partial: binding completion callback state is the corresponding mechanism')
    if 'task_wait_registration_t' in s:return ('Await registration, continuation and scheduler delivery record', 'Yes; through continuation resumption', 'No same Framework wait registration; binding has awaitable result state')
    if 'operation_registry_state_t' in s:return ('Pending operation entry, identity/deadline and terminal callback', 'Yes; through reply, admission failure, timeout or cancellation terminal', 'Partial: binding completion owner tracks operation completion, not Framework operation registry')
    if 'materialize_binding_part' in s:return ('Heap raw_bytes_t ownership wrapper and external native message handle; bytes are moved into retained storage', 'Yes; native message storage is retained until transport releases it', 'Yes, at the public message/external-message ownership boundary')
    if 'encode_application_payload' in s:return ('Serialized payload byte vector (about 1.34 KiB requested/message at this site)', 'Normally no; it is consumed into message parts before submit returns', 'No same Framework service-wire encoder; binding caller provides message parts')
    if 'encode_node_request_header' in s or 'encode_reply_header' in s:return ('Temporary encoded header byte vector', 'No; bytes are moved into the message before return', 'No Framework header format in the binding benchmark')
    if 'application_payload_t::from_parts' in s or 'message_parts_t::message_parts_t' in s:return ('Multipart message_parts_t/vector and message_t handles', 'Container is moved/released during submit; native message buffers can outlive submit', 'Partial: binding represents parts as vector<message_t>')
    if 'envelope_codec_t::encode_header' in s or 'canonical_header_reader_t::string' in s or 'client_call_codec_t::create_envelope' in s or 'create_reply_header' in s:return ('Header strings, metadata and encoded envelope storage', 'Usually transient until the native message is built; native bytes persist after submit', 'No equivalent Framework envelope codec; binding request metadata has separate storage')
    if 'raw_route_port_t::request' in s:return ('Binding request submission/result wrapper and completion callback captures', 'Yes; pending reply/admission storage lasts until terminal', 'Yes; this is the Framework adapter over binding request operations')
    if 'raw_route_port_t::reply' in s:return ('Reply multipart/vector and native message ownership wrappers', 'Yes; through send completion/transport ownership', 'Partial: binding has message parts and one-way send')
    if 'raw_route_port_t::receive_if_ready' in s or 'raw_route_port_t::poll' in s:return ('Receive event/result record, message vector and route-poll state', 'Receive-side only; retained to dispatch boundary, then released/moved', 'No Framework poll wrapper; binding/Core poller is lower-level')
    if 'raw_mesh_node_owner_t::request_with_header' in s or 'request_admission_completion' in s or 'deliver_request_terminal' in s:return ('Request callback/lambda captures, operation state and coroutine/continuation context', 'Yes; callback/context lives until admission or physical reply terminal', 'Partial: binding request operation and completion entry own lower-level terminal state')
    if 'route_client_t::submit_request_reply_message_erased' in s:return ('Mixed: route/packet strings, metadata/envelope vectors, payload encoder and reply decoder closures, shared target owner', 'Mixed: encoding temporaries end during submit; decode/target captures live through reply terminal', 'Partial: binding has operation/serializer inputs but no route client or Framework reply projection')
    if 'route_client_t::request_to_node' in s or 'public_host_runtime_t::request_to_node' in s or 'mesh_node_runtime_t::submit_node_request_reply' in s:return ('Typed call/request task state, receiver and routing/coroutine captures', 'Yes; async result and routing state survive until completion', 'Partial: binding async_result_t request has reply/admitted stages')
    if 'request_submit_operation_t::async' in s:return ('Binding async request operation state and reply/admitted result handles', 'Yes; until binding completion/admission terminal', 'Yes; this is the binding implementation')
    if 'completion_entry_t::capture' in s or 'resume_async_slot' in s or 'completion_owner_t::drain_impl' in s:return ('Binding completion entry, captured result and resume slot/scheduler closure', 'Yes; through completion delivery/continuation resume', 'Yes; binding run executes the same completion path')
    if 'public_host_runtime_t::dispatch_ready' in s or 'service_mailbox_t::try_enqueue' in s or 'mesh_record_dispatcher_t::dispatch' in s:return ('Ready/receive record, mailbox queue node and dispatch callback captures', 'Target-side: through application dispatch/handler completion', 'No matching Framework target dispatch in binding server run')
    if 'next_receive_activity_async' in s or 'node_receive_role_t::rotate' in s or 'next_dispatch_activity_async' in s:return ('Coroutine frame and receive/dispatch activity result state', 'Target-side: until activity arrives or coroutine resumes', 'No matching Framework receive scheduler')
    if 'dispatch_application' in s or 'dispatch_request' in s or 'route_handler_invoker_t::invoke' in s or 'invoke_filters_async' in s or 'mesh_channel_server_builder_t::add_handler' in s or 'handler_registry_t' in s:return ('Handler dispatch task/coroutine frame, route context and handler/filter callable storage', 'Target-side: through handler/result completion', 'No matching Framework handler/runtime in the binding server run')
    if 'deserialize_typed_payload' in s or 'channel_request_call_t::decode' in s:return ('Typed reply/request decoded payload object and result wrapper', 'Reply side: result payload survives to caller; temporary decode state ends at completion', 'Partial: protobuf payload type exists in binding benchmark, but Framework typed projection does not')
    if 'decode_application' in s or 'make_route_message_context' in s:return ('Temporary part-view/container or route metadata/context strings', 'Decode view is transient; context can live through handler dispatch', 'No equivalent Framework service-wire receive conversion')
    if 'classify_node_target' in s:return ('Target classification/lookup temporary state (small strings or container growth)', 'No; lookup ends during submit', 'No Framework topology resolver in binding path')
    if 'service_scope_t::create' in s:return ('Handler service-scope owner/control block and scoped provider state', 'Target-side: through handler completion', 'No Framework service scope')
    return ('Framework runtime object/callable inferred from the named function; exact C++ allocation type is not tagged by this counter', 'See path role: task/request objects persist to terminal; encode/decode temporaries do not', 'No direct binding object at this Framework layer')

object_rows=[]
for label,rec in sorted(site_records.items()):
    if label.startswith('Core native') or 'benchmark caller' in label: continue
    info=object_info(label)
    if info: object_rows.append((label,*info))

parts=[]
parts.append('''# C++ Framework request submit allocation-site census — issue #3867

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

''')
parts.append(calls_table('framework','submitter','Framework source submitter (critical thread)'))
parts.append(calls_table('framework','source','Framework whole source process'))
parts.append(calls_table('framework','target','Framework whole target process'))
parts.append(calls_table('binding','submitter','Binding run source submitter (comparison)'))
parts.append('''### Scope totals

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

The counter records allocation size and stack, not C++ dynamic type. The object column below is inferred from the call path in source. “Past submit” refers to ownership after the caller's submit routine returns; target-side rows instead say whether state survives through dispatch. Equivalent binding status describes the binding benchmark path, not necessarily the same public abstraction.

| Framework/binding site | Allocated object(s) | Past submit? | Binding equivalent |
|---|---|---|---|
''')
for label,obj,life,equiv in object_rows:
    parts.append(f'| `{label}` | {obj} | {life} | {equiv} |\n')
parts.append('''
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
''')
(J/'report.md').write_text(''.join(parts))
print('wrote',J/'report.md','object rows',len(object_rows))
