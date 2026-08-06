<!-- framework-adapter-nav:start -->
[Document List](../../../README.en.md) | [Previous: Backend Policy](backend-dependency-policy.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)<!-- framework-adapter-nav:end -->

[Common layering](../../common/internals/01-layering.en.md) | [Common spec](../../common/spec/README.en.md)

# .NET Runtime Integration and Receive Ownership

This document fixes where the `.NET Framework` runtime uses the public API of
`bindings/dotnet` and what responsibility remains in a semantic adapter. A reader should be
able to decide from this document whether a type is only forwarding arguments or is actually
translating Framework and binding meaning, ownership, lifecycle, and concurrency.

This document does not add a Framework public contract. The Framework public contract is owned
by the [common spec](../../common/spec/README.en.md) and the .NET exact-interface documents.

## 1. Layering rule

The `.NET` implementation follows this responsibility graph. The `Framework public/domain
contract` and `Framework semantic runtime core` do not know binding types or binding options.
Code that calls binding types stays inside the last two layers.

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
| `ZLinkBackendContextWrapper` | Create, shutdown, and dispose `IContext` | It binds context identity and Framework context lifetime into one port. | It owns context lifetime relative to sockets created from it. | Lifecycle only. | Keep semantic adapter. |
| DEALER/ROUTER socket adapter | Fluent `Send`, `Request`, and `Reply` | Framework uses direct operations and `bool`/callback results; the binding uses builder submission. | It owns the socket operation owner relative to close. | A gate is used for send/request/receive/reply. | Keep semantic adapter; removing the gate requires separate concurrency proof. |
| DEALER/ROUTER receive | `Recv(Received, RecvFlags)` | The caller supplies storage instead of receiving a new envelope object. | Storage is not reused while an async queue owns it. | No `Received.Create()` per receive. | Keep caller-provided-storage adapter. |
| PUB/SUB socket adapter | Fluent publish and `Subscribe(TopicMessage, ...)` | Framework topic/message operations are translated to a binding builder and subscription result. | The queue owns `TopicMessage` until dispatch completes. | A topic storage pool is used. | Keep semantic adapter. |
| monitor adapter | `MonitorEvent`, `ISocketMonitor.Recv` | Native events, timeout, and results are converted to Framework monitor events. | It owns monitor disposal and callback lifetime. | Monitor array and nonblocking poll storage are reused. | Keep semantic adapter. |
| socket poller | `IPoller.Poll(PollEvent[])` | Binding event arrays become Framework readiness flags. | The owner disposes the poller and event array together. | One `PollEvent[1]` is reused for the lifecycle. | Keep semantic port. |
| `ZLinkBackendSpotNodeWrapper` / `ZLinkBackendSpotWrapper` | `IMeshNode`, `ISpot`, and dispatch callbacks | A binding mesh object becomes Spot, Actor, completion, and lifecycle meaning. | It owns completion tables, dispatch pump, and Spot/Actor lifetime. | Only required semantic translation is performed. | Keep semantic adapter. |
| `ZLinkBackendStreamSocketWrapper` | `IStreamSocket`, `IStreamSessionService` | Raw frames and bound-actor sessions become one Framework Stream meaning. | It owns session/socket shutdown order and shared MeshNode ownership. | `_sendGate` preserves session submit order. | Keep semantic adapter. |
| `ZLinkRawRouterServicePort` / `ZLinkManagedMeshNode` raw receive | Public `IRouterSocket.Recv(Received, ...)` | This path handles service wire directly without another Framework application envelope. | One receive owner holds storage; the binding resets it on the next receive. | Caller-provided `Received` and reusable event arrays. | Direct public binding call. |
| `SetChannelName` on backend sockets | None | Channel name is a Framework domain/config value and had no binding socket meaning. | It stored no value and changed no lifecycle. | It added a call, validation, and fake method only. | Remove pass-through. |

The DEALER/ROUTER socket adapters do not re-export binding sockets as Framework public APIs.
They are internal boundaries that translate Framework submit results, option mapping, and
lifecycle rules to the binding public builders.

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
or lifecycle rule.

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

The rejection path returns ownership when the queue is full or cancellation occurs. Therefore,
storage in a pool is no longer used by the queue, handler, or reply callback. Conversely, a path
such as `ZLinkRawRouterServicePort` that stores an envelope in a separate record must not share
one storage instance. It uses storage whose lifetime matches the retained envelope.

`DispatchWork` is a value type stored by the queue. It removes one heap work-record allocation
per message. Delegates and tasks are still used where the asynchronous handler requires them;
they are not added to the binding receive operation itself. Native message-part allocation
follows the binding ownership contract, and the Framework does not copy the same bytes again.

## 5. Lifecycle and concurrency

The DEALER/ROUTER adapter `_gate` is not a facade for the binding fluent builder. It is a
lifecycle guard that makes Framework receive, reply, send/request, and socket disposal use one
socket owner. Binding submit serialization is separate; Framework must also define an execution
order for raw receive and close. Removing this gate requires tests and measurements that prove
all of the following:

- the public binding contract permits concurrent receive, reply, request-callback registration,
  and dispose;
- a close that rejects an in-flight public operation has a defined Framework owner that joins it;
- throughput, p99 latency, and lock contention improve against the baseline after removal.

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
- Pass-through contracts such as `SetChannelName` do not return.

The basic build commands are:

```text
dotnet build framework/languages/dotnet/src/Zlink.Framework/Zlink.Framework.csproj --nologo
dotnet build framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj --nologo
```

Where the native runtime is available, run the backend factory, client-server channel, fanout,
stream-concurrency, and receive-dispatch-budget tests together. Release approval also compares
throughput, p99 latency, allocation/GC, and lock contention against the pre-change baseline. An
unexplained regression is not a completed result.

## 7. Regression Tests

The following tests pin the decisions in this document:

| Test | Pass criteria |
|---|---|
| `BackendAdapterFactoryTests.BackendFactory_Creates_Channel_Spot_And_Stream_Wrappers` | The backend factory creates the semantic adapters required by the Framework without exposing binding objects in the public surface. |
| `ClientServerChannelRuntimeTests.BackendWrappers_DeliverUnsolicitedLivenessProbe` | A caller-provided `Received` envelope is reused for control receive and is not retained by the application queue. |
| `InboundDispatchBudgetTests.Dispatch_queue_rejects_when_full_without_blocking_receive_loop` | A full application queue rejects its owned envelope and keeps the control receive loop nonblocking. |
| `BackendStreamSocketConcurrencyTests.ConcurrentBoundActorMessages_AreSubmittedSerially` | The Stream semantic adapter preserves the single submit owner required by the binding session service. |
| `RegressionTests.DotNetContractDocuments_AllExposeRegressionTestSection` | The document remains part of the .NET formal-document regression set. |

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../../../README.en.md) | [Previous: Backend Policy](backend-dependency-policy.en.md) | [Next: Regression Test Matrix](regression-test-matrix.en.md)<!-- framework-adapter-nav:bottom:end -->
