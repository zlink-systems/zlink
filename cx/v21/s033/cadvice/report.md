# C++ request and send execution design — #3867 / #3868

## Recommendation

Keep caller-executed submission. Evaluate **receive-role transfer at receive-rotation boundaries**, using one process execution-resource owner for application work and reserved completion work. Preserve independent completion items and the existing owner queues. This can remove the target receive-to-handler transfer and the source receive-to-completion transfer for a batch whose receive role can be assigned to an idle worker. It must fall back to the present worker delivery when that assignment is unavailable.

**This is a recommendation for removing Framework transfers, not evidence of a path to 0.6.** The normal C++ source submission already executes on the caller, including binding submission under the socket mutex. The source has no routine caller-to-submit-worker hop to delete. Source CPU and transport service demands remain too large even under generous removal assumptions. A dedicated socket reactor would add a submission transfer; combining its work with the current receive role would worsen the request capacity bound.

The first deliverable should be a small placement experiment with a stop condition, before a general executor rewrite. Do not start that rewrite on an assumed 3.77 switches × wake-latency saving.

## Evidence and scope

Source reading used /root/worktree/zlink-3867a, branch perf/3867-submit-alloc, HEAD 5a1f381edd22c08cbdfacda0aa5ce8c0394f145a. It contains the wire writer and allocation changes. Measurement used only the existing main binaries in /root/worktree/zlink-ratio040, ee1139025782688e33109455041d8a0222cc8fe3. Source locations below refer to the advice worktree; profile symbols refer to main. Their execution structure agrees on the paths discussed here.

Read first:

- [Main request stage profile](../cprof/report.md), including its exclusive stages, thread table and counter limits.
- [Allocation census](../calloc/report.md) and [stacked allocation fixes](../sfix/report.md).
- [Earlier R/W/T/M/H analysis](/root/jobs/fx/cx/ratio034/gap14-analysis.md). R was subsequently implemented by #3600. W and the allocation changes are already present in the advice source; their savings cannot be credited again.
- The complete Korean submit/completion, handler/gate and messaging-hot-path specifications; state-lane ownership; the referenced system design principles. The applicable prose and POSDDD performance principles were also consulted.

The new measurement fills the missing send thread/stage evidence. It is one profile per implementation, send-saturation 1024, 10 s warmup, 20 s active, approximately 8 s capture. The job-local runner preserves the existing CPU masks, validation, admission behavior, payload and logical concurrency. Command:

~~~sh
flock -o /root/jobs/fx/cx/measure.lock bash /root/jobs/fx/cx/v21/s033/cadvice/run.sh
~~~

SKIP_BUILD=1 throughout. No builds, model calls, sub-agents, repository edits, push or external messages were made. Minimum sampled MemAvailable was **25.930 GiB**, sampled once per second. The existing four runner edits in ratio040 were preserved. See [preflight](preflight.json), [profile log](profile.log), [memory samples](memory.tsv), [analysis](analysis.log), and raw captures under out-profile/.

Both new send cells passed: submitted = completed = final received, zero errors, rejections and abandoned messages; no contamination or drain-bound hit. The count validator also accepted the original Framework output and rejected a job-local copy with received increased by one: [positive/negative results](count-validation.json). No runtime test suite was run for this read-only advice.

### Source notation

Paths are relative to /root/worktree/zlink-3867a:

| Prefix | Directory |
|---|---|
| C/ | framework/languages/cpp/framework/src/runtime/ |
| P/ | framework/languages/cpp/framework/include/zlink/framework/contracts/ |
| B/ | framework/bench/grpc/cpp/ |
| D/ | framework/languages/dotnet/src/Zlink.Framework/Runtime/ |
| J/ | framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/ |
| N/ | framework/languages/node/packages/framework/src/runtime/ |
| X/ | framework/doc/framework/common/spec/server/01-execution/ |

## Contracts and decision ownership

The common specifications deliberately distinguish a new execution turn from a different thread. Resource placement may change; completion authority, admission and queue order may not.

| Decision | Required owner and clause | C++ decision locations | Cross-language locations | Owner retained in the recommendation |
|---|---|---|---|---|
| Source target selection | Selector state lane; X/08 §3 E2. Explicit RID has no weighted selection. | C/mesh/mesh_node_runtime.cpp:4441 dispatch_node_request path; C/mesh/raw_mesh_node_owner.cpp:1548 peer lookup. Channel selection uses the topology selector. | D/Service/ZLinkManagedMeshNode.cs:10757 TrySelectChannelTargetAsync; J/binding/ZLinkJavaRawMeshNode.java and its topology selector; N/foundation/raw-service-mesh-runtime.ts:725 selectChannelTarget. | Existing selector. The scheduler does not choose or replace targets. |
| Registration, deadline index and close acceptance | Operation state lane, X/01 §§9–10.1; X/06 §§4–5; X/08 E3/E4. | C/mesh/raw_mesh_node_owner.cpp:1543–1603 request owner turn; :1627 start_send; C/foundation/operation_registry.cpp:345 register_operation. | D/Service/ZLinkManagedMeshNode.cs:11038 CreateOperationAsync and :5586 implementation; J/internal/service/ZLinkServiceOperationRegistry.java:329 submitTerminal; N/foundation/raw-service-mesh-runtime.ts:1805 registerRequest, N/foundation/operation-registry.ts. | Existing operation owner. No registration queue added. |
| Logical first terminal | One atomic take at notification, X/01 §§9–10.1; X/08 E5. | C/foundation/operation_registry.cpp:489 take, :508 complete; C/mesh/raw_mesh_node_owner.cpp:269 deliver_request_terminal. | D/Service/ZLinkManagedMeshNode.cs:11111 CompleteManagedOperation and TryRemoveOperation; J/internal/service/ZLinkServiceOperationRegistry.java:404 complete/:416 completeExceptionally; N/foundation/operation-registry.ts:332 complete/:349 fail/:434 take. | Existing registry. No completed flag, second pending table or owner-lane return trip. |
| Physical admission, HWM retry, native completion | Core/binding, X/01 §§5,7,15 and system design principles “제어 결정과 요청 수락의 소유”. | C/backend/raw_route_port.cpp:199 send .async(), :390 request .async(); bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:279 submit_request_attempt, :668 drain_impl. | D/Messaging/ZLinkRequestSubmissionOutcome.cs:69 operation.Async; J/binding/ZLinkJavaRawServicePort.java:450 submit; N/backend/node/node-raw-binding-port.ts:698 submitRawRequest. | Core/binding unchanged. No Framework writable waiter, retry or extra poller. |
| Delivery resource and new turn | Process completion dispatcher and execution-resource owner, X/01 §11; X/08 §4.4. | C/foundation/operation_registry.cpp:230 post_completions; C/dispatch/provider_task_context.cpp:174 dispatch_completion; C/dispatch/offload_executor.cpp:172 start_role. | D/Backend/DotNet/ZLinkCompletionDispatcher.cs:14 Post; J/internal/service/ZLinkServiceCompletionDispatcher.java:58 post; Node Promise reactions, N/backend/node/node-raw-binding-port.ts:633 observeRequestReply. | One placement owner; registry and dispatcher retain completion reservation/lifetime. |
| Handler execution rights and order | Owner queue plus execution gate, X/02 §§2–9,13; X/08 I1–I4/W1–W5. | C/mesh/service_mailbox.cpp:497 take_application_receive_turn; C/mesh/mesh_node_host_service.cpp:3769 drain_application_owner; existing serial execution queues for Spot/Actor. | Corresponding language application lanes/gates; Node event-loop turns and Java/.NET worker drains. | Existing queue/gate. Resource assignment conveys no handler permission. |

The following conditions apply to every alternative:

- E1 encoding stays synchronous on the calling resource. Required wire writes and typed serialization remain.
- E3/E4 remain one operation-owner turn; requests register receiver, identity, reply route and dispatcher reservation before binding submit. Pending sends register after the binding result and before consuming admitted. Immediate sends create no pending entry or E5 delivery.
- A request uses one monotonic deadline including selection/owner-turn wait, readiness, admission and reply. Remaining time goes to binding. Deadline expiration and reply race at the existing terminal take; no second reply-time expiration rule.
- Send has no timeout or caller cancellation. Binding operation starts once and retains its selected target while waiting. No Framework send backlog substitutes for admission.
- X/01 §9 preserves physical receiver/codec cleanup after an earlier public terminal. Admission and reply remain independently observable, isolated between calls.
- Completion callbacks and suspended continuations start outside completion locks, lane-current scopes and the producing stack, following X/01 §11 and X/08 §4.4. Explicit caller schedulers and ordinary await/Yield gate behavior remain.
- Node/Channel pipeline starts remain FIFO; their independent handlers may overlap. Spot/Actor gates, Actor claims, lifecycle priority and time budgets remain unchanged.
- Permit precedes ordinary receive, returns immediately before handler entry, and does not restrict preidentified terminal completion. A rotation claims up to 64 within the other limits and checks management once. Infrastructure progresses while application handlers wait.
- Reply encoding and binding reply start stay on the completing handler's execution resource, X/08 W4.
- Accepted completion reservations survive through callback return and drain on shutdown. Completion callbacks remain independently schedulable.

X/08 §7 currently specifies 0.90 for its listed throughput cells. The requested 0.6 is an interim investigation target, not a relaxation of that specification.

## Hop map

These are established-peer, explicit-RID RouteMesh calls from the actual benchmark. They do not exercise weighted ChannelName selection, local-target dispatch, relocation or cancellation on the successful path.

**A function call, lane entry, coroutine or notification is not necessarily a thread transfer.** Context switches below count scheduler events on a resource, not a correlated message edge. CPU per message is normalized by all completed messages; pool rows do not mean every message executes on every worker. Per-hop queue delay and wake-to-run latency were not measured in the supplied CPU/counter evidence or the new CPU capture.

### Request-backpressure 1024

~~~text
source caller S -- binding request.async(), same S --> Core source TX
  -- TCP --> Core target RX --> target Framework receive role R
  --> target application worker A -- binding reply.submit(), same A -->
Core target TX -- TCP --> Core source RX
  --> source Framework receive role R'
  -- binding poller completion drain, same R'; logical terminal take -->
process completion worker D -- typed result / benchmark notification --> caller S
~~~

An immediate admitted-stage observation also schedules a benchmark notification. It is a side branch, not a transfer required to get the request to the socket.

| Boundary | File:function, queue/lane/executor | Purpose | Main profile cost: CPU µs/message; resource switches/message |
|---|---|---|---|
| Caller → E1 encode | B/client/bench_cpp_client.cpp:878 submit, :940 run_tasks; P/dispatch/request_task.hpp:479 receive; C/channels/channel_runtime.cpp:1736 submit_request_reply_message_erased | Prepare final receiver; protobuf/envelope encode; deadline begins before encoding. Same caller thread. | Whole submitter **23.405**, **0.0481** switches in CPU capture. Exclusive allocation 7.559, bookkeeping 5.057, binding/Core 5.004, codec 2.626, synchronization 1.473, tasks 0.781, driver 0.905. |
| E2/E3/E4 on caller | C/mesh/mesh_node_runtime.cpp:4399 submit_node_request_reply → C/mesh/raw_mesh_node_owner.cpp:1522 request_with_header → state_lane_t::run_sync | Explicit RID, one operation-owner turn. Idle lane is claimed by caller. On contention a FIFO grant wakes the same caller; another worker does not normally perform its synchronous submit. | Owner lambda inclusive **10.700** inside 23.405. Lane-related cost is included; neither 10.700 nor synchronization is an independently removable hop cost. Grant frequency/wait latency unknown. |
| Caller → binding socket call, still caller | C/backend/raw_route_port.cpp:351 request_impl, :375 socket mutex, :390 submit_request_once; binding request_reply.cpp:106 async → completion_owner.cpp:585 start_async_request | Short protocol gate serializes native submission versus close; binding owns physical operation/admission. Registry already registered. | Included in caller binding/Core 5.004. **Zero normal Framework thread transfers before socket call.** |
| Source socket → Core source TX | core/src/runtime/core/mailbox.cpp:338 schedule_if_needed_unlocked; runtime/engine/asio/asio_engine.cpp:680 start_async_write | Core transport mailbox/reactor, not a Framework submit executor. | Source IO/0 TID 1106953: **19.494**, **0.3598**. Stack contains 16.263 inclusive start_async_write and 14.464 inclusive __sys_sendto; those overlap. |
| Network → Core target RX → Framework receive role | asio_engine.cpp:453 start_async_read; C/backend/raw_route_port.cpp:404 poll/:459 receive_if_ready; C/mesh/mesh_node_host_service.cpp:2088 run/:2097 rotate | Core network receive, then public binding poller; I0 readiness, pre-receive permit and ordinary claim/classification. | Target Core IO/0 1106884: **15.533**, **0.4613**. Target receive 1106889: **16.986**, **0.4572**. These are different resources, not additive edge latency. |
| Target receive → application worker | C/mesh/service_mailbox.cpp:497 take_application_receive_turn/:516 notify_application_ready → C/mesh/mesh_node_host_service.cpp:3282 submit_owner → C/dispatch/offload_executor.cpp try_submit_internal; :3769 drain_application_owner | Owner queue ready publication, one worker transfer for the drain. W1/W2/W3; permit returned before handler. No second mailbox or per-record supervisor handoff here. | Two active application workers 1107183/1107482: **11.047 / 11.716**, switches **0.0998 / 0.1058**. Combined CPU **22.764** per message across their work. Ready-drain fanout/batch-size distribution not captured. |
| Handler → reply socket call, still application worker | C/mesh/mesh_record_dispatcher.cpp:31 dispatch → C/channels/route_packet_dispatcher.cpp:347 dispatch_request → route_handler_invoker.cpp:10 invoke/:63 completion → C/mesh/raw_mesh_node_owner.cpp:2022 reply → raw_route_port.cpp:501 reply/:516 submit | Same handler turn encodes reply and starts binding reply. A synchronous port snapshot uses run_sync at :2030; contention grants the application caller its turn. | Included in application worker CPU. **No routine reply-worker transfer.** No claim that the whole worker CPU is removable. |
| Reply socket → Core target TX → network → Core source RX | Core transport engine and separate RouteMesh Completion connection | HWM-free ROUTER–ROUTER reply progression, independent of ordinary application permit/PAUSED state. | Target IO/1 1106885: **18.884**, **0.3713**. Source IO/2 1106955: **15.247**, **0.4772**. |
| Core completion → source Framework receive resource, binding drain inline there | bindings/cpp/src/Runtime/Eventing/poller.cpp:404 completion_owner->drain → Messaging/completion_owner.cpp:668 drain_impl → async_operation_state.hpp:51 resume_async_slot; C/backend/raw_binding_adapter.hpp:147 observe_request_completion | Binding owns completion_recv and WRITABLE retry. Observer has an empty continuation scheduler, so raw classification remains on the poller resource; no Framework executor before terminal selection. | Source receive TID 1106958: **18.350**, **0.3800**. Includes poller/management/physical classification, not just raw reply decoding. |
| Logical terminal → completion worker | C/mesh/raw_mesh_node_owner.cpp:1393 observe_request/:269 deliver_request_terminal → C/foundation/operation_registry.cpp:489 take/:230 post_completions → C/dispatch/provider_task_context.cpp:174 dispatch_completion | Atomic first-terminal selection at notification; pre-reserved dispatcher item; E5 new turn. C1 take does not return to the operation lane. | Eight completion workers together **25.984**, **1.6338** switches/message in CPU capture. Each contributes about 3.24–3.26 µs normalized by all messages. Includes result delivery, task/notification work, allocation and synchronization. |
| Completion worker → caller observation | C/mesh/mesh_node_runtime.cpp:4406 receiver callback → composed service-wire/envelope/protobuf decode → P/dispatch/request_task.hpp receiver/public state → P/dispatch/task.hpp:1376 observe_task_completion → B/common/bench_async.hpp:46 notify → B/client/bench_cpp_client.cpp:953 collect/validate | No additional decoder executor. Benchmark uses completion_signal_t deque/mutex/CV to notify its one submitting thread. Explicit application schedulers can add their own posts outside Framework placement counts. | Caller validation/collection is included in submitter total. Return notification latency not measured. Source caller switches are small because it is normally busy submitting. |
| Admitted-stage side observation | B/client/bench_cpp_client.cpp:942–946 always observes preceding_admission, including ready admission | Benchmark next-submit sequencing. Ready value could be consumed without a notification; pending admission still needs observation. Observer uses captured runtime scheduler if available, otherwise process completion dispatcher. | H enclosing critical scope **1.118** from existing profile, including necessary reply observation. Exact ready-only removable time and worker attribution unknown. This is not another mandatory request-result hop. |

The binding comparison calls its binding async request on its submitting thread and drives its own public poller/ready-queue there. It does not have Framework receive, application-dispatch, logical registry or E5 workers. Both paths still have Core transport threads.

### Send-saturation 1024

~~~text
caller S -- E1, owner turn, binding send.async(), all S --> Core source TX
  -- TCP --> Core target RX --> Framework receive R --> application worker A

immediate admission: result consumed on S; no public reply and no E5
pending admission: Core readiness --> binding poller on source receive R'
  --> registry take --> completion worker --> benchmark pending-send notification to S
~~~

| Boundary | File:function and owner | Cost in the new main send CPU profile |
|---|---|---|
| Caller encode → owner turn → binding send, same caller | B/client/bench_cpp_client.cpp:881 send_to_node.async; C/mesh/raw_mesh_node_owner.cpp:1727 send_with_header_result/:1759 submit_owned_send_result/:1627 start_send/:1637 start_send_on_lane; C/backend/raw_route_port.cpp:150 submit_send_impl/:183 socket mutex/:199 async | Submitter TID 2268474 **5.414 µs/message**, **0.983 cores**, **0.00035** switches/message. Allocation 1.598; synchronization 0.217; tasks 0.157; codec 0.952; binding/Core 1.001; bookkeeping 1.019; driver 0.463. start_send_on_lane inclusive **1.236**, send_with_header_result inclusive **2.080**; nested scopes overlap. |
| Immediate admission → caller result consumption, same caller | raw_route_port.cpp:224 returns ready result; raw_mesh_node_owner.cpp:1657 immediate branch; bench_async.hpp:46 appends already-ready send to local completed list | No pending registry/E5 or ready-send driver notification. Not an opportunity to delete another completion hop. |
| Source Core TX | Core mailbox → asio_engine_t::start_async_write | IO/0 TID 2267872 **4.990 µs**, **0.906 cores**, **0.09296** switches/message. |
| Target Core RX → Framework receive | Core start_async_read → raw_route_port poll/receive → node_receive_role_t::rotate | Core IO/0 2267807 **3.807 µs**, **0.691 cores**, **0.15732** switches; Framework receive 2267812 **4.284 µs**, **0.778 cores**, **0.11521** switches. |
| Target owner queue → application worker → one-way handler | Same mailbox/submit_owner/drain path as request, then route_packet_dispatcher::dispatch_send and route_handler_invoker::invoke. Synchronous void benchmark handler updates counts; no reply | Workers 2267813/2268476 **2.771 / 2.751 µs**, **0.00518 / 0.00516** switches. Waking is already strongly amortized here. The two worker contributions sum to **5.522 µs/message**, not an extra result latency. |
| BACKPRESSURED branch only | raw_route_port.cpp:228 observe_send_completion; raw_mesh_node_owner.cpp:1674 register_operation before :1701 terminal observation; existing completion dispatcher and driver signal | Same physical/logical ownership and new-turn rules as pending request admission. New capture did not resolve a meaningful per-message source receive/completion-worker contribution. Pending fraction is not instrumented; do not declare it zero or charge a request-like E5 to every send. |

New send capture rates were F **181,538** / B **609,943**, ratio **0.298**. Full active profiled rates were 175,350 / 606,998, ratio **0.289**. These single-run, different-window figures do not replace ratio040's three-run medians.

Whole-process CPU was source/target **10.592 / 13.772 µs/message** for Framework and **2.845 / 2.485** for binding. Framework source/target switches were **0.10860 / 0.28883**; binding **0.02699 / 0.09697**. Binding's busiest send source resource in this capture was Core IO/0, **1.556 µs/message**; its application submitter was **1.273**. Thus the request cell's 3.7 µs binding submitter must not be used for send arithmetic.

### Switches, wake latency and transport cost

The earlier separate request counter run observed **3.7749 switches/message across both Framework processes versus 0.2002 binding**, and **11.4117 versus 1.6203 futex calls/message**. It observed source/target syscall totals 18.4747/10.2878 versus 1.9966/1.4103. Source sendto was 0.8833 versus 0.1171 per message; target sendto 0.9113 versus 0.1830. The CPU-capture resource switches in the hop table are from a different window/instrumentation and sum differently; they must not be mixed with those counter totals.

Neither evidence measures a causal wake cost for a specific edge. Dividing CPU by switches does not supply wake latency; multiplying switches by a guessed context-switch time does not supply removable throughput spacing. The transport syscall excess may partly reflect weaker batching from slow submission. It does not prove that executor placement alone will eliminate it.

## Cross-language caller-to-socket comparison

| Runtime | Caller to binding socket | Reply and completion placement | Consequence for C++ |
|---|---|---|---|
| .NET | D/Service/ZLinkRawRouterServicePort.cs:136 RequestAsync creates messages and calls SubmitAndAwaitReplyAsync at :153 before its first unresolved await; SendAsync does the same at :84. A search found no runtime call sites constructing this port; ManagedMeshNode is the relevant direct mesh path. Its RequestToNodeDirectAsync at :10696 builds wire on caller, RequestDirectWireAsync at :11018 completes operation registration through AwaitStateLane, and SubmitNativeWireAsync at :11294 enters _socketGate and invokes binding at :11314. ZLinkRequestSubmissionOutcome.cs:69 calls operation.Async synchronously. State lane RunAsync has caller execution when idle at D/Execution/ZLinkStateLane.cs:90–115. | Async physical observation can resume on binding/.NET scheduling resources; it is not guaranteed to stay on the original thread. CompleteManagedOperation takes the logical entry, then delivery uses the process ThreadPool dispatcher, one independent work item per completion. .ConfigureAwait(false) is not a dedicated submit-thread transfer. | Caller submission under a short socket gate already has a C++ counterpart. The raw port alone is insufficient evidence for the whole .NET request path. |
| Java / Kotlin runtime | J/binding/ZLinkJavaRawMeshNode.java:454 creates one shared port/operation state lane. ZLinkServiceOperationRegistry.java:344 registers and starts on that owner turn; application callers use owner.awaitTurn at :279–282. J/internal/execution/ZLinkStateLane.java:126 awaitTurn claims idle work or waits for a FIFO grant and executes its own turn. ZLinkJavaRawServicePort.java:441 inStateLane then :450 binding submit; sendMessages does the same at :208–222. Runtime callers can use the lane executor instead. | Dedicated platform pump drives receive and binding completion, not normal caller submission. Terminal take at ServiceOperationRegistry:404 posts the reserved entry to ServiceCompletionDispatcher:58, whose common-pool items remain independent. Binding/CompletionStage scheduling is additional language-owned work. | A dedicated Java receive pump is not a precedent for moving C++ submissions onto a socket thread. Same logical contract permits different physical resources. |
| Node | N/backend/node/node-raw-mesh-backend.ts:820 calls raw-service-mesh-runtime.ts:680 requestToNode → :1689 requestToTarget, registering the operation and using the port. N/backend/node/node-raw-binding-port.ts:335 request calls submitRawRequest at :347/:358, which invokes binding submit at :698. send/submitSend at :323/:329 also invoke binding synchronously. All execute on the same JS event-loop thread. | Binding Promise reply reactions at :633 perform physical result processing; public Promise continuations run as later microtasks on the same JS resource. No second JS worker can receive a transferred role. | Same-resource later turns are valid, but C++ cannot run callbacks while retaining its receive role by appealing to Node's event loop. X/08 §4.4 explicitly gives the single-resource runtime its existing path. |

Observable parity means identical admission, first terminal, deadline, queue order, gate rights, result isolation and shutdown outcomes. It does not require all languages to use identical worker counts or to adopt a C++ placement optimization. If the experiment needs a new observable rule, it becomes classification D and needs supervisor specification work plus parity changes, not a C++ exception.

## Alternatives and ceiling arithmetic

### Capacity model

For a pipeline with serialized resource demand d_i CPU µs/message, available resource count n_i and binding rate B:

~~~text
U_i = n_i × 10^6 / (B × d_i)
ratio ≤ min(U_caller, U_owner, U_Core-thread, U_aggregate, ...)
~~~

These are conditional CPU-capacity bounds, not latency predictions. They freeze the stated service demands; changed contention/batching can change those demands. A bound above the goal is necessary, not sufficient.

Ratio040 inputs and calculations are retained in [calculations.json](calculations.json):

| Quantity | Request BP 1024 | Send saturation 1024 |
|---|---:|---:|
| F / B msg/s | 36,314.766 / 220,467.251 | 163,272.781 / 542,277.748 |
| Measured ratio | 0.16472 | 0.30109 |
| 0.6 goal rate | 132,280.351 | 325,366.649 |
| Current spacing → goal spacing, µs | 27.537 → 7.560 | 6.125 → 3.073 |
| Caller CPU C, ratio040 | 26.138 | 5.730 |
| Profile caller CPU | 23.405 | 5.414 |
| Profile-to-ratio040 scaling k = C/profile C | 1.11680 | 1.05826 |
| Frozen caller bound 10^6/(B C) | **0.17353** | **0.32184** |
| Dominant source Core thread, profile | 19.494 | 4.990 |
| Illustrative scaled Core bound 10^6/(B k IO) | **0.20834** | **0.34918** |

These main-based ceilings are not post-#3888 throughput predictions. Scaling a profile's other resource by its caller's k assumes stable relative service demands. That assumption is unverified. The direct matched-capture bounds give the same conclusion: request caller 10^6/(271112 × 23.405) ≈ **0.158**, source Core ≈ **0.189**; send caller 10^6/(609943 × 5.414) ≈ **0.303**, source Core ≈ **0.329**.

Request aggregate constraints matter too. At the ratio040 goal, eight CPUs allow **60.478 CPU µs/message per endpoint**. Existing source/target demands are **107.967 / 82.943**. A faster caller alone does not fit either endpoint into its CPU allocation.

The supplied stacked-fix report measured +15.69% request throughput and allocations 49.341 → 24.283, but contains no new per-stage CPU table. Halving allocations does not prove that allocator CPU halves. Using the question's approximate **20 µs post-fix caller** gives a caller-only bound of **0.227 at B=220,467**, or **0.185 at B=270,366**. The corresponding 0.6 budgets are **7.560 / 6.165 µs**, respectively. Main and post-fix stage savings are not added together below.

### A. Caller-owned inline submission and socket gate

**Structure.** Encode on caller; enter the FIFO operation lane; register request; invoke binding under the existing short socket gate; consume ready admission directly. Use the queued lane grant only on contention.

This is substantially the current design: state_lane.hpp:289 run_sync, state_lane.cpp:134 schedule_drain and raw_route_port.cpp:375/:183. It removes zero normal submission transfers from today's workload. The actionable residual is the benchmark's unnecessary observation of an already-ready admitted stage. That removes a side notification and observer storage, not an E2/E4 worker hop.

**Contract mapping.** Existing owner turn keeps registration/close/identity ordered, binding submission synchronous within that turn, and completion outside the lane. The existing FIFO empty check and drain claim must stay atomic. A caller fast path cannot ignore queued predecessors or allow recursive entry.

**Forbidden variant.** Calling the port directly under _socket_mutex while bypassing the operation lane does not preserve C2 ownership of registration, deadline index and close. X/06 §§4–5 forbid replacing that ownership with a mutex/semaphore. Retaining the lane but duplicating its pending/closed decision under a second gate also fails the single-owner design rule. A short socket protocol gate is valid; using it as a substitute operation state machine is not.

**Risks.** Caller draining unrelated infrastructure work can lengthen submission; contended grants still have real wake delays. Inline ready-result consumption must preserve stage isolation. It must not turn an application completion callback into inline execution on a lane/transport stack. Socket close remains serialized by the existing protocol gate. A pending-ready observer registration must use the task's race-safe observer mechanism.

**Cross-language consistency.** This matches the idle .NET lane, Java awaitTurn and Node synchronous binding invocation. No common contract change is justified.

**Ceiling.** Without changing service demands: request **0.174**, send **0.322**. Even deleting the entire critical synchronization stage gives:

~~~text
request: 10^6 / [220467.251 × (26.138486 − 1.472778 × 1.116802)] = 0.18518
send:    10^6 / [542277.748 × (5.729816 − 0.216575 × 1.058262)] = 0.33525
~~~

Those optimistic deletions include mandatory synchronization and more than the ready-admission opportunity. No additional transfer elimination is established. A is the retained submission baseline, not a new route to 0.6.

### B. Receive-role transfer with shared resource placement

**Structure.** Keep A on submission. At the end of I0–I4 and management for a rotation, offer the receive role to an idle resource through the executor that owns resource assignment. If it reserves a resource exclusively for that role, the old receiver may run eligible application/dispatcher work in a fresh turn after its poller/drain/lane scopes have returned. Otherwise it remains receiver and leaves the work available to other workers.

On target, this can turn R → application-worker into a later turn on R. On source, it can turn R' → completion-worker into a later turn on R'. The next receiver is a different resource, so the receive-role wake is moved/amortized, not eliminated from system accounting. No handoff is attempted for each record.

Use the existing application owner queue and the request's reserved completion item. Bulk-publish independently schedulable completion items under the shared scheduler's queue operation; do not capture a batch in a callback loop whose next callback depends on the previous callback returning. The old receiver can take one eligible turn while other ready items remain visible to other workers.

**Contract mapping.** This is the placement explicitly permitted by X/08 §4.4, with X/01 §11 stack/scope separation. Single poller, permit/cursor/pending-management/shutdown state remain with the role. Existing gates and FIFO still decide whose application turn may begin. The binding drain still chooses physical results and retries; registry take still occurs at notification. Explicit caller schedulers remain honored and may still require a resource transfer.

**What might forbid a proposed implementation.**

- An executor post or waiting worker is not an exclusive idle-resource assignment. Both fail §4.4's success condition.
- Running application work before the old receive/drain stack returns fails §11.
- Holding the receive role while invoking a callback fails infrastructure/application progress separation.
- Sending the completion batch to a new broker queue before the dispatcher adds an unallowed E5 transfer.
- A serial callback batch hides unrelated callbacks behind one callback; §4.4 forbids that dependency.
- Combining all handlers onto the receiver reduces available handler concurrency, contrary to §4.3.
- Importing binding internals or adding a second poller would cross the layer boundary.

**Risks.** Role ownership transfer, shutdown and fallback are the principal correctness risks. A late notification must still have exactly one poller owner. Accepted completion items cannot be released when the resource changes. Saturation may leave no idle resource, making the optimization inactive precisely under load. More assignments can increase cache movement of the poller/role, and aggressive local work may reduce receive batching. A blocked callback must retain only its worker, not the new receiver or all completion items. Source callers still allocate and submit on a separate thread.

**Cross-language consistency.** Physical worker assignment is language discretion under X/08 §6. .NET/Java keep their current placement while parity tests check the same observable outcomes. Node keeps event-loop microtasks. No new public contract, admission behavior or error classification is required.

**Ceiling.** For the placement change alone, caller CPU is unchanged: request **≤0.174**, send **≤0.322** in the ratio040 model. Even with enough benefit to remove caller synchronization completely, the bounds are only **0.185 / 0.335**. The existing request receive/worker costs cannot be subtracted from caller CPU.

A deliberately excessive sensitivity bound deletes **all** caller allocation and synchronization, including required allocations:

~~~text
request caller: C' = 26.138486 − (7.559080 + 1.472778) × 1.116802
                  = 16.05169 µs; Ucaller = 0.28258
send caller:    C' = 5.729816 − (1.598171 + 0.216575) × 1.058262
                  = 3.80933 µs; Ucaller = 0.48409
~~~

Keeping the measured transport service demand caps even those cases at approximately **0.208 / 0.349**. This is a sensitivity envelope, not an expected saving from B. Reaching 0.6 requires further measured reductions in caller codec/bookkeeping/native work and Core transport demand. The post-fix ~20 µs caller also rules out 0.6 from a purely off-caller placement change.

A naïve implementation that executes all target receive plus handler work serially on one fixed thread makes capacity worse. At profile demands, request receive + application work is 16.986 + 22.764 = **39.750 µs/message**; send is 4.284 + 5.522 = **9.806**. Role transfer must preserve worker concurrency and cannot be sold as a fixed single-thread event loop.

### C. Dedicated infrastructure reactor owning socket submission and lane

**Structure.** Caller performs E1, then an owner-lane RPC asks one infrastructure reactor to perform E3/E4 and return the binding result; that reactor also drives the existing public poller, deadline visits and physical completions. Publish reserved completions in batches to independent process workers.

This can remove lane-versus-port cross-resource access and some protocol-gate contention. It **adds** caller → reactor and acknowledgement transfers to normal submission. A synchronous caller still cannot observe submission return until required registration and submit have occurred. An ordinary asynchronous send queue returning before binding admission is forbidden by E4 and X/01 §4; the state-lane queue cannot be relabeled to evade that boundary.

**Contract mapping and blockers.** A reactor's owner turn could satisfy C2 FIFO, registration and close if all relevant writers truly move there and completion take remains C1 at notification. It must not run application callbacks while remaining receiver. E1 may not move to the reactor. A strictly sole-thread socket owner would also require handlers to enqueue replies to it, violating **W4's same completing-resource reply start**. A compliant reduced variant must retain direct handler reply under the existing socket protocol gate, so it is no longer sole-thread socket ownership.

The reactor must not absorb Core transport engines via private Framework access. Fusing Framework and Core transport ownership is a different Core/binding design, not an executor optimization supported by current package APIs.

**Risks.** Every submit now contends for a serialized reactor and returns through a second scheduling event. Saturated receive/drain can delay registration; deadline includes that delay. Public handler callers cannot be made to block waiting for infrastructure progress. Close must order accepted turn RPCs before socket/poller disposal. Reply direct access retains the protocol gate. Independent callbacks still require other execution resources.

**Cross-language consistency.** The dedicated Java pump is receive infrastructure, not evidence for this submission arrangement. .NET and Node normally start native operations from the caller. A physical owner placement change could preserve semantics; an early-return send queue, reply-worker queue or changed completion context could not.

**Ceiling of the reduced variant.** Optimistically move the entire observed owner scope, ignoring RPC/wake overhead. Request owner work is 10.700 µs and existing source receive work 18.350:

~~~text
new caller = (23.404757 − 10.6998) × 1.116802 = 14.1889 µs
Ucaller = 0.31967
reactor = (18.349664 + 10.6998) × 1.116802 = 32.4425 µs
Ureactor = 0.13981
end ratio ≤ min(0.31967, 0.13981, Core 0.20834) = **0.13981**
~~~

Send start_send_on_lane has 1.236 µs enclosing scope:

~~~text
new caller = (5.414365 − 1.236) × 1.058262 = 4.42180 µs
Ucaller = 0.41704
reactor submit-only lower demand = 1.236 × 1.058262 = 1.30801 µs
end ratio ≤ min(0.41704, Core 0.34918, reactor/target limits) ≤ **0.34918**
~~~

The send receive role has too few samples for a source service-demand estimate; treating it as zero here is an optimistic lower demand, not a measured absence. Request may improve if costs collapse when colocated, but that is unmeasured and would need to overcome at least 32.443 → 7.560 µs reactor demand, before scheduling overhead. A literal sole-socket-thread variant is rejected under W4 regardless of its hypothetical throughput.

### Comparison

| Design | Normal submission transfer removed | Other transfer affected | Conditional main-based ceiling: request / send | Verdict |
|---|---|---|---|---|
| A caller lane + socket gate | 0; already present | Ready admitted-stage side notification only | **0.174 / 0.322** unchanged CPU; ideal deletion of all caller synchronization **0.185 / 0.335** | Retain; do not recreate it as a new optimization. |
| B rotation-boundary role transfer | 0 | Target receive→handler and source receive→dispatcher can become later turns on same resource; new receiver still must wake | **0.174 / 0.322** from placement alone; no supported 0.6 estimate | Recommended limited experiment. |
| C dedicated reactor lane | Adds caller RPC | Colocates registration and physical completion; still needs callback workers and direct replies | **≤0.140 / ≤0.349** under optimistic relocated enclosing costs | Reject for these cells; strict sole socket owner also conflicts with W4. |

No candidate has a defensible 0.6 forecast from the available evidence. Changing B's frozen CPU assumptions is a measurable hypothesis, not a reason to report 0.6 as its ceiling.

## Recommended implementation sequence

This is a future implementation plan, not authorization to change code in this read-only task. The supervisor should approve classification A against the existing resource-placement clauses before runtime work.

1. **Establish a post-#3888 demand baseline.** Keep the same explicit-RID requests and eight logical send slots; record caller, receive, application and Core-thread CPU separately. Do not repeat R/W or infer a CPU saving from allocation counts. The current main send profile is the read-only diagnostic result for this task.
2. **Target-only placement experiment.** Refactor C/mesh/mesh_node_host_service.cpp node_receive_role_t::run/rotate into a rotation plus executor-owned continuation boundary. Extend existing role assignment in C/dispatch/offload_executor.hpp/.cpp to reserve an idle worker for the next receive role. Finish the old receive scope before the old resource may take eligible application work. Preserve service_mailbox.cpp's single owner-ready queue and drain claim. No new mailbox, per-record supervisor or public tuning option.
3. **Evaluate target result before source integration.** This experiment must show fewer message resource transfers after I4 while maintaining independent reception and handler concurrency. Record role-assignment wake cost too. If send caller/IO demand remains limiting, do not continue on a claim that target placement will achieve 0.6.
4. **Integrate source completion placement through one resource owner.** Move the execution-resource allocation currently split between offload_executor and completion_dispatcher_t's private Asio workers into the same process owner. Keep the existing process dispatcher and pre-reserved operation_completion_item_t as the completion authority/lifetime objects. C/dispatch/provider_task_context.cpp, completion_turn.hpp and C/foundation/operation_registry.cpp:230 are the boundary. Replace per-item queue locking/notification with bulk publication of existing independent items. Remove the superseded private worker allocation; do not leave two competing placement mechanisms or add a broker between binding notification and dispatcher.
5. **Preserve caller schedulers.** Review C/dispatch/coroutine_executor.cpp and P/dispatch/task.hpp context capture/reuse. They must still restore caller execution context, ordinary handler turns and Yield reacquisition. Value conversion is allowed within the selected completion turn, not a second generic task chain. P/dispatch/request_task.hpp's independent admission/public/physical outcomes stay intact.
6. **Shutdown and ownership audit.** Review C/backend/raw_route_port.cpp poll/close lifetime and C/mesh/mesh_node_host_service.cpp stop/drain order. Role state owns socket/poller/cursor/permits/management through transfer. Operation registry take and binding retry remain untouched. Audit the changed call paths for duplicate gates, queues, flags, temporary logging and unused dispatch helpers before final validation.

The ownership simplification is resource placement in **two current mechanisms → one process placement owner**: offload application/role scheduling and private completion worker scheduling. Terminal winner **1 → 1**, operation C2 owner **1 → 1**, binding retry owner **1 → 1**, execution gate **1 → 1**. These are decision locations in the stated scope, not a count of all runtime rules. Replace the old completion worker list/resource allocation and role scheduling mechanism rather than adding a second assignment authority. Current read-only work changes all runtime counts by **0**.

### Validation order

Use existing tests as anchors, adding behavior-focused cases where missing:

| Stage | Test locations and required observations |
|---|---|
| Placement primitive | tests/Zlink.Framework.UnitTests/test_cpp_framework_state_lane.cpp and test_cpp_framework_execution.cpp: idle and contended FIFO; no lane reentry; scopes gone before callback; close drains accepted turns. Existing lane behavior must remain unchanged. |
| Receive-role boundary | test_cpp_framework_receive_role_completion.cpp: one poller owner; no idle busy polling; 64 ready records batch; no data permits while completion still progresses; a deliberately blocked application callback does not stop the transferred receiver, deadline visits or peer acceptance. Test assignment failure with no idle resource as well as success. |
| Completion independence and lifetime | test_cpp_framework_completion_dispatcher_owner.cpp plus existing dispatcher shutdown/lifetime tests: block completion A and verify unrelated B runs on another eligible worker; drop the last owner while accepted items remain; repeated shutdown/start; callback exceptions do not discard other work. |
| Admission / deadlines / first terminal | test_cpp_framework_raw_route_port_contract.cpp, test_cpp_framework_backpressure_reliability.cpp, task completion-source tests: immediate request reply before submit returns; BACKPRESSURED admitted/reply sequencing; missing token rejection; one deadline including owner wait; reply/timeout/cancel/shutdown race; late physical cleanup once; isolated stages across submissions. |
| Queue/gate behavior | Existing Node/Channel FIFO-start and async-filter tests; SpotWide/PerActor turn exclusion, ordinary await retention, Yield reacquisition and self-request rejection. Fresh DI scope per dispatch and same-resource W4 reply submission. Multiple gates retain concurrency. |
| Cross-language contract | Existing cross-language request/send scenarios with .NET, Java/Kotlin and Node peers: admission completion, deadline/late reply, shutdown, error kind and sequence checks. Resource placement assertions remain C++-specific; public outcomes remain common. |
| Final gates | Focused tests first; then complete C++ unit/contract labels and source-string target_contract checks once after the implementation stabilizes. Follow with the requested performance cells and existing multi-cell regression coverage. No CI-driven iteration. |

For new placement checks include a negative control: retain the receive role while running the blocked callback, or falsely count a queued task as a successful handoff, and require the progress assertion to fail. For completion independence, deliberately capture multiple callbacks in one serial batch and require the blocked-A/unrelated-B test to fail. Do not weaken queue/order assertions or increase timeouts to obtain green tests.

## Ceiling experiment before full implementation

The baseline already disproves **0.6 from deleting only the measured submitter synchronization cost**. It also disproves an unchanged-CPU B design as a complete throughput solution. It does not disprove indirect allocator/batching benefits; those require an experiment.

The smallest useful future experiment is step 2's **target-only rotation transfer**, followed by a narrow source placement experiment with existing completion items. Neither needs a general lane rewrite, wire change, public option, retry policy or new completion state.

Measure under the same global flock and CPU masks, with the installed packages verified. Keep request/send admission, payloads, validation and logical concurrency fixed. Use interleaved A/B runs and independent binding reference runs. Collect:

- Resource identities at caller entry, E3, E4, physical notification, registry take, dispatcher item start, handler start and W4; identify the resource separately from lane/turn. Count task posts, role transfers and kernel switches separately.
- Queue-publication-to-first-instruction latency and wake-to-run latency, with p50/p95/p99 and batch size. Existing CPU captures cannot supply these. Use existing flow events where available and job-local scheduler tracing where they are not, with a tracing-disabled comparison.
- Caller CPU and each serialized receive/Core resource's CPU per validated message; aggregate endpoint CPU; allocator CPU and calls; sendto/recvfrom/epoll/futex per message; actual records per receive rotation and bytes/messages per native write.
- A blocked-callback progress run, resource high-water and shutdown drain, followed by uninstrumented throughput.

At ratio040 binding rates, a proposed 0.6 ceiling needs a caller and every single serialized resource below **7.560 µs/request** and **3.073 µs/send**, and total endpoint CPU below **60.478 / 24.588 µs/message**, respectively. In the faster profile cohorts those single-resource budgets tighten to approximately **6.15 / 2.733 µs**. A resource near the budget with no headroom is insufficient to predict the observed ratio.

**Disproof condition:** measured post-fix caller demand stays near 20 µs/request or send demand near 5 µs; an unchanged Core thread stays above its matching budget; a fixed owner-drain remains serialized above budget; role transfer seldom succeeds; or role wakes cost as much as the removed transfer. Any of those rejects a 0.6 claim for the experiment's scope. Preserve a smaller improvement only if its complexity is justified independently.

**Support condition:** resource demands meet all matching budgets, transferred-receiver progress and independent completion tests pass, transfer/wake counts fall after including role wakes, and repeated uninstrumented F/B measurements approach the modeled capacity with full count validation. Only that combination would justify the larger integration. Reduced switches or a faster isolated queue microbenchmark alone would not.

## Open questions

- **Post-fix attribution:** which part of the ~20 µs caller remains after #3888, and how much is allocator contention versus necessary codec, logical registration and native work? Current post-fix allocation counts do not answer this.
- **Role assignment availability:** how often is an eligible idle resource available under request backpressure and send saturation? Current C++ roles are persistent; no existing success/failure census can be reused.
- **Completion composition:** what shares of the 25.984 µs request completion-pool total belong to E5, the benchmark's ready admitted-stage observation, payload conversion and allocator/scheduler work? It is not all removable dispatcher overhead.
- **Core batching causality:** does faster Framework submission reduce its per-message transport syscall demand toward binding? Framework cannot compensate for a binding/Core defect with retries, alternate routes or another poller.
- **Owner-drain serialization:** the two active target application workers do not prove that their full CPU demands can run concurrently for one logical Node queue. Measure pipeline-start/drain occupancy before assigning a two-core capacity credit.
- **Resource owner implementation:** can the existing offload allocator absorb independent completion items while retaining current worker-management bounds and a guaranteed infrastructure receiver? If not, source integration requires a separate reviewed design, not cross-pool idle guesses.
- **Observer and scheduler behavior:** a caller's explicitly chosen execution context can retain its own post even after Framework placement is optimized; the benchmark's signal back to its caller remains necessary for pending replies.
- **Language resource documentation:** B uses §4.4's existing allowance, but exact C++ resource mapping documentation may need supervisor updates within protected paths. This advice does not authorize or perform those edits.

Result: report and send profile evidence only. Repository changes: none. Functional tests executed: none; benchmark count validation 2/2 plus positive/negative validator checks passed. Remaining runtime failures from this task: none. Design classification: **A, if B stays within existing placement rules**; strict sole-thread socket ownership conflicts with W4, and early-return admission or altered delivery semantics would require **D**. Audit findings: no implementation audit claimed; the alternatives identify the existing caller fast path, avoidable ready-admission observation, and placement/transport measurements still needed.
