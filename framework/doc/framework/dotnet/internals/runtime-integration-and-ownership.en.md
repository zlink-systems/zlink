<!-- framework-adapter-nav:start -->
[Document List](../../../README.en.md) | [Previous: Backend Policy](backend-dependency-policy.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)<!-- framework-adapter-nav:end -->

[Common layering](../../common/spec/40-internal-layering.en.md) | [Common spec](../../common/spec/README.en.md)

# .NET Runtime Integration and Receive Ownership

This document fixes where the `.NET Framework` runtime uses the public API of
`bindings/dotnet` and what responsibility remains in a semantic adapter. A reader should be
able to decide from this document whether a type is only forwarding arguments or is actually
translating Framework and binding meaning, ownership, lifecycle, and concurrency.

This document does not add a Framework public contract. The Framework public contract is owned
by the [common spec](../../common/spec/README.en.md) and the .NET exact-interface documents.

## 1. Layering rule

The `.NET` implementation follows this responsibility graph. The `Framework public/domain
contract` never exposes binding types or binding options. The internal contracts under
`Runtime/Backend/Contracts` are the binding-facing seam at the bottom of the Framework runtime,
not a Framework public/domain model: they may carry the binding's public `Message`, `Received`,
`TopicMessage`, and flag types when that preserves caller-provided storage and zero-copy
ownership. These types must stop at this seam and must not spread into Framework public APIs,
semantic application models, or binding-independent documentation. Meaning, lifecycle,
readiness, error, and concurrency translation remains in the adapter implementation that owns
the seam.

```text
+----------------------------------------------+
| Framework public/domain contract             |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Framework semantic runtime core              |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Binding-facing runtime integration           |
| direct public call or semantic adapter       |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Systems.Zlink public API                     |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Core                                         |
+----------------------------------------------+
```

If the binding operation and the Framework operation have the same meaning, ownership,
lifecycle, readiness, error, and concurrency rules, the binding public API is called directly
from binding-facing integration. If any one differs, the difference is kept in one semantic
adapter or port. A class name or current implementation in another language is not evidence for
this choice.

Some current source file names still contain `Wrapper`. The types retained in the table below
are not pass-through wrappers that expose binding objects. Each one either translates a
Framework meaning to a binding public operation or composes several binding objects into one
Framework lifecycle.

## 2. Implementation classification

In the table, `keep` means keep a semantic adapter or port. `direct call` means use the
`Systems.Zlink` public API directly from binding-facing runtime integration. `remove` means
remove an internal Framework surface that only forwards the same input.

| Type/path | Binding operation | Semantic mismatch | Ownership/lifecycle | Hot-path cost | Decision |
|---|---|---|---|---|---|
| .NET runtime context port | Create, configure, create children, and dispose `IContext` | Semantic runtime needs one generation-scoped resource owner, while `IContext` and binding option mapping belong to binding integration. | The port owns context configuration and is disposed after all child sockets and services have stopped. | Lifecycle and creation only; no message hot-path work. | Keep `IZLinkBackendRuntimeContext`; binding public calls stay inside its .NET implementation. |
| DEALER/ROUTER socket adapter | Fluent `Send`, `Request`, and `Reply` | Framework uses direct operations and `bool`/callback results; the binding uses builder submission. | The Framework runtime owns one receive consumer and joins it before socket close; the binding/Core lifecycle contract reports `EBUSY` for a close that races an admitted operation. | Independent send/request/reply submissions use the binding/Core concurrency path; the Framework adapter adds no per-operation gate. | Keep semantic adapter for result and builder translation; direct binding concurrency contract. |
| DEALER/ROUTER receive | `Recv(Received, RecvFlags)` | The caller supplies storage instead of receiving a new envelope object. | Storage is not reused while an async queue owns it. | No `Received.Create()` per receive. | Keep caller-provided-storage adapter. |
| PUB/SUB socket integration | Socket configuration, poller creation, and `ISubSocket.Subscribe(TopicMessage, ...)` | Configuration and readiness are translated. The semantic ingress fixes the nonblocking receive form and supplies caller-owned storage. | The queue owns `TopicMessage` until dispatch completes. | The topic storage pool is reused; the ingress adds no Framework envelope. | Keep semantic ingress port; call binding `Subscribe` only inside the binding adapter. |
| monitor adapter | `MonitorEvent`, `ISocketMonitor.Recv` | Native events, timeout, and results are converted to Framework monitor events. | It owns monitor disposal and callback lifetime. | Monitor array and nonblocking poll storage are reused. | Keep semantic adapter. |
| socket poller | Public `IPoller.Wait(Span<PollEvent>, TimeSpan)` mapped to `ZLinkBackendSocketReadiness` | Binding event flags become a Framework readiness contract. | The owner disposes the poller and event array together. | One binding `PollEvent[1]` is reused for the lifecycle. | Keep semantic port; binding flags stop at the .NET adapter. |
| `ZLinkBackendSpotNodeWrapper` / `ZLinkBackendSpotWrapper` | `IMeshNode`, `ISpot`, and dispatch callbacks | A binding mesh object becomes Spot, Actor, completion, and lifecycle meaning. | It owns completion tables, dispatch pump, and Spot/Actor lifetime. | Only required semantic translation is performed. | Keep semantic adapter. |
| `ZLinkSpotNodeInitializer` → `ZLinkBackendSpotNodeWrapper` → `ZLinkManagedMeshNode` | `MaxMessageSize`, directional HWM, mailbox caps, and directional timeouts | Converts the Framework `IZLinkMeshNodeSocketConfig` into managed ROUTER socket options without merging directions. | Values reach the node before startup bind and are applied when the socket is created. | Startup-only path; no message hot-path work. | Keep the semantic adapter path; keep send and receive values separate. |
| `ZLinkBackendStreamSocketWrapper` | `IStreamSocket`, `IStreamSessionService` | Raw frames and bound-actor sessions become one Framework Stream meaning. | It owns session/socket shutdown order and shared MeshNode ownership. | `_sendGate` preserves session submit order. | Keep semantic adapter. |
| `ZLinkRawRouterServicePort` / `ZLinkManagedMeshNode` raw receive | Public `IRouterSocket.Recv(Received, ...)` | This path handles service wire directly without another Framework application envelope. | One receive owner holds storage; the binding resets it on the next receive. Async request completion remains with the binding progress pump. | Caller-provided `Received` and reusable event arrays; the receive poller does not also claim `PollCompletion`. | Direct public binding call inside backend integration. |
| `SetChannelName` on backend sockets | None | Channel name is a Framework domain/config value and had no binding socket meaning. | It stored no value and changed no lifecycle. | It added a call, validation, and fake method only. | Remove pass-through. |

The runtime context port is not a second public `Context` surface. It owns one binding context
for one Framework runtime generation, applies the Framework HWM policy, creates the backend
children, and closes the context after those children have stopped. Semantic runtime code sees
only `IZLinkBackendRuntimeContext` and backend contracts; it cannot access `IContext` options.

The DEALER/ROUTER socket adapters do not re-export binding sockets as Framework public APIs.
They are internal boundaries that translate Framework submit results, option mapping, and
lifecycle rules to the binding public builders. They do not serialize independent message
operations a second time. The subscriber ingress contract is part of this binding-facing seam:
its `TryReceive(TopicMessage)` operation fixes the poll-loop's nonblocking receive contract
without allocating a second Framework envelope, while the binding adapter maps it to the public
`ISubSocket.Subscribe` operation. The `TopicMessage` parameter is binding caller-owned storage,
not a Framework public/domain type.
`ISubSocket` never crosses the semantic backend contract.

### 2.1 MeshNode ROUTER option handoff

`ZLinkSpotNodeInitializer` passes the topology's `IZLinkMeshNodeSocketConfig` to the backend
node once. `ZLinkBackendSpotNodeWrapper` moves it into directional `IMeshNode` properties,
and `ZLinkManagedMeshNode` applies `SendHighWaterMark`, `ReceiveHighWaterMark`, `SendTimeout`,
and `ReceiveTimeout` to their corresponding binding options when it creates the ROUTER socket.
Copying the send high-water mark into the receive option or dropping the receive timeout would
change the common topology contract.

`BackendAdapterFactoryTests.SpotNode_Router_Send_Config_RoundTrips_Through_Binding` checks that
the send and receive high-water marks and both timeouts remain separate. The test pins the
source-level mapping; package and Windows process verification are separate evidence.

## 3. Comparing a direct call with an adapter

Non-trivial operations are designed with at least two alternatives before implementation.

| Alternative | Decision left in callers | Assessment |
|---|---|---|
| Direct binding public call | Each runtime caller must know builder stages, binding options, raw event mapping, and close order. | Use only for raw service paths whose meaning is exact; otherwise decisions leak upward. |
| Argument-forwarding `*Wrapper` | Caller complexity does not decrease; only classes, methods, and test fakes increase. | A shallow module; remove it. |
| Semantic adapter/port | One place decides Framework result, error, ownership, and lifecycle meaning. | Choose when the difference is proven by code and tests. |

The existence of a binding object such as `IContext`, `IRouterSocket`, or `IStreamSocket` is
not by itself a reason to keep every adapter. Conversely, an interface is not removed merely
because it has one implementation. First establish whether it owns a Framework-domain meaning
or lifecycle rule. `IZLinkBackendRuntimeContext` is retained because it owns a runtime-generation
resource boundary and the child-creation/disposal order; it is not a method-for-method context
facade.

The following forms are forbidden:

- a `*Wrapper` with the same arguments and results as the binding object
- a one-implementation `IBackend*` added only for testability
- a facade that only renames binding methods
- a second copy of the binding option surface
- raw-frame encode/decode in Framework callers
- binding internal/private access, reflection, or direct native symbols

## 4. Receive storage ownership

`Received` and `TopicMessage` are caller-provided storage for message envelopes. After a
successful receive, the envelope owns its message parts. Every consumer that reads the current
message must retain that storage until the next successful receive would overwrite it.

### 4.1 Synchronous processing paths

The client control loop and the `ZLinkManagedMeshNode` receive loop process a message in the
same execution flow. Each lifecycle owns one `Received.Create()` value and repeatedly passes it
to `Recv(storage, RecvFlags.DontWait)`. On success, the binding resets old parts and metadata
and fills the new result. The next receive is called only after processing ends, so overwriting
the storage cannot affect an earlier consumer.

### 4.2 Asynchronous application dispatch

The client-server and fanout receive loops can hand a message to an application queue. The
receive loop cannot reuse the same storage until the queue worker finishes the handler.
`ZLinkReceivedStoragePool` and `ZLinkTopicMessageStoragePool` enforce this sequence:

1. The receive loop rents storage from the pool.
2. A binding public receive operation fills the storage.
3. A control message is processed in the same loop and the storage is returned.
4. An application message transfers storage ownership to the queue item.
5. After the handler or rejection callback finishes, parts are disposed and storage is returned.

The topic-message pool calls the binding public `TopicMessage.ReleaseForReuse` operation when
it returns storage. This releases received parts and metadata but retains the binding's topic
receive buffers. Terminal callers still use `Dispose`, which also releases those buffers.

The receive loop keeps the current storage as its owner until `PostAsync` returns. A rejection
callback then returns the storage to the pool, and a successful post transfers it to the queue.
If measuring the payload, reserving budget, or posting fails before that return, the receive-loop
`finally` block still owns and returns the storage. This ordering prevents a handoff-before-queue
exception from leaking an envelope.

The rejection path returns ownership when the queue is full or cancellation occurs. Therefore,
storage in a pool is no longer used by the queue, handler, or reply callback. Conversely, a path
such as `ZLinkRawRouterServicePort` that stores an envelope in a separate record must not share
one storage instance. It uses storage whose lifetime matches the retained envelope.

`DispatchWork` is a value type stored by the queue. It removes one heap work-record allocation
per message. Delegates and tasks are still used where the asynchronous handler requires them;
they are not added to the binding receive operation itself. Native message-part allocation
follows the binding ownership contract, and the Framework does not copy the same bytes again.

## 5. Lifecycle and concurrency

The binding public socket contract permits independent send, request, and reply builders to be
submitted concurrently on one socket. The binding and Core preserve each multipart submission as
one operation; they do not require the Framework to add a second hot-path lock. Receive remains a
single-consumer operation, so each Framework receive loop has one explicit owner and never shares
its `Received` or `TopicMessage` storage with another receive call.

Close and dispose use Core's stricter lifecycle gate. A close that races an admitted operation or
callback can report `EBUSY`; the Framework stops the relevant poller, receive loop, submitter, or
request-progress owner before closing and retries in the ClientServer connection path where a
request callback is still being released. The DEALER/ROUTER adapters therefore do not add a
second `_gate` around send, request, reply, receive, or dispose. This removes duplicate lock
contention while preserving the actual binding/Core ownership rules.

The public binding concurrency contract and the request/reply and mixed-submission regression
tests in `test_socket_concurrency` pin this decision. They cover multipart send, concurrent
request and reply, and concurrent send/request submission. If a future binding changes the
contract, the binding contract and tests must be updated before changing Framework integration.

The Stream adapter `_sendGate` serializes `IStreamSessionService.SendToActor` submission. A
concurrency test requires that concurrent bound-actor sends reach the binding session service
one at a time, so the gate stays until a different single owner is defined.

The 10ms `Task.Delay` used during client connection close is not a message hot path. It is a
bounded lifecycle retry after an in-flight request callback has finished. It must not become a
per-message await in receive or send operations.

## 6. Verification criteria

An implementation change is checked against all of these conditions:

- The Framework public assembly does not expose binding sockets, `IContext`, or `IMeshNode` as
  public contract.
- Framework source uses no binding internal/private member or reflection.
- Caller-provided `Recv` and `Subscribe` storage is not reused before the queued handler ends.
- Poll event arrays, receive envelopes, and topic envelopes are reused within their safe lifetime.
- Framework callers do not assemble binding send/request builders directly.
- DEALER/ROUTER hot-path calls do not add a Framework lock above the binding/Core concurrency
  contract.
- Pass-through contracts such as `SetChannelName` do not return.

The basic build commands are:

```text
dotnet build framework/languages/dotnet/src/Zlink.Framework/Zlink.Framework.csproj --nologo
dotnet build framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj --nologo
```

Where the native runtime is available, run the backend factory, client-server channel, fanout,
stream-concurrency, and Application Job Queue admission tests together. Release approval also compares
throughput, p99 latency, allocation/GC, and lock contention against the pre-change baseline. An
unexplained regression is not a completed result.

## 7. Regression Tests

The following tests pin the decisions in this document:

| Test | Pass criteria |
|---|---|
| `BackendAdapterFactoryTests.BackendFactory_Creates_Backend_Resources_Through_Runtime_Context` | The backend factory creates sockets, Spot, and Stream resources through one generation-scoped semantic context port; the binding context stays inside the .NET integration boundary. |
| `BackendAdapterFactoryTests.SpotNode_Router_Send_Config_RoundTrips_Through_Binding` | MeshNode topology's MaxMessageSize, directional high-water marks, mailbox caps, and directional timeouts reach the managed ROUTER socket options without losing their direction. |
| `ClientServerChannelRuntimeTests.BackendWrappers_DeliverUnsolicitedLivenessProbe` | A caller-provided `Received` envelope is reused for control receive and is not retained by the application queue. |
| `ApplicationJobQueueAdmissionTests.Saturation_waits_before_receive_and_terminal_completion_bypasses_capacity` | Ordinary ingress acquires a host-wide permit before receive/claim. Saturation is a cancellable wait with no reject, drop, busy spin, or unbounded side queue; only a pre-receive-identifiable terminal reply/error completion may bypass the permit so completion remains live. The reservation is returned at the handler's actual first instruction, and cancellation/teardown leaks no permit or envelope owner. |
| `test_pubsub.pubsub_topic_message_can_be_released_for_reuse` | The binding public reuse operation clears received parts while allowing the same topic envelope to receive the next publish. |
| `test_socket_concurrency.dealer_and_router_allow_concurrent_public_sends` | Independent public DEALER and ROUTER send builders complete concurrently without a Framework duplicate gate, while each receive remains single-consumer. |
| `BackendStreamSocketConcurrencyTests.ConcurrentBoundActorMessages_AreSubmittedSerially` | The Stream semantic adapter preserves the single submit owner required by the binding session service. |
| `RegressionTests.DotNetContractDocuments_AllExposeRegressionTestSection` | The document remains part of the .NET formal-document regression set. |

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../../../README.en.md) | [Previous: Backend Policy](backend-dependency-policy.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)<!-- framework-adapter-nav:bottom:end -->
