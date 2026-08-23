---
title: ".NET Bindings Implementation Blueprint"
---

<!-- bindings-nav:start -->
[Spec index](../README.en.md) | [Previous: C](../c/README.en.md) | [Next: C++](../cpp/README.en.md)
<!-- bindings-nav:end -->

# .NET Bindings Implementation Blueprint

> **What this chapter defines** — the `Contracts`/`Runtime` shape the .NET
> library must have, and the baseline architecture map other wrapper
> bindings reference.

This document defines the shape the .NET library must have. It is not an
exhaustive list of every interface member. The actual public contract
source lives at `bindings/dotnet/src/Zlink/Contracts/`.

The .NET implementation is considered aligned once `Contracts/`, the
runtime implementation classes, tests, samples, the perf runner, and
package behavior all follow this blueprint and map `core/include/zlink.h`'s
stable features onto a .NET-appropriate API.

This README describes the finished .NET binding shape — it is not a
temporary target draft. It also serves as the baseline guide for aligning
other wrapper binding documents to the same architecture map. Even when
another binding uses its own language-specific naming, the contract/
runtime ownership, public contract categories, file-splitting criteria,
and verification intent described here still apply.

This binding follows the common bindings architecture map using .NET
naming. `Contracts/<Category>` owns the public contract source and
`Runtime/<Category>` owns the implementation. Another binding may use
different casing or package names, but this document is that same map
projected onto .NET.

The code a reviewer reads first should be the public contract under
`Contracts/`. A runtime file must implement that contract, and a new
user-facing behavior should never be discovered first in a runtime file.

| Section | Covers |
|---|---|
| [Public contract source](#public-contract-source) | Namespace, contract/runtime source locations, the API reference link |
| [Repository layout](#repository-layout) | The aligned directory tree and folder ownership boundary |
| [API change workflow](#api-change-workflow) | The procedure for new mappings/refactors, and the shortcuts that must be removed |
| [Library shape](#library-shape) | Interface/concrete-type classification, builders, `IDisposable`, RoutingId helpers |
| [Contract / Runtime placement rules](#contract--runtime-placement-rules) | The boundary between public declarations and runtime implementation |
| [Standard interface rules](#standard-interface-rules) | recv signatures, builder start methods, naming constraints |
| [Contract folder layout](#contract-folder-layout) | The ownership scope of each category under `Contracts/` |
| [Runtime folder layout](#runtime-folder-layout) | The implementation scope of each category under `Runtime/` |
| [Creation entry points](#creation-entry-points) | The list of public factory methods |
| [Required feature coverage](#required-feature-coverage) | The user-facing features that must be guaranteed once aligned |
| [Receive and Subscribe shape](#receive-and-subscribe-shape) | Caller-provided storage and distinguishing no-data |
| [Service and SPOT shape](#service-and-spot-shape) | The split of responsibility between `ISpotNode`/`ISpot` |
| [Byte HWM and monitoring ABI v4](#byte-hwm-and-monitoring-abi-v4) | `ulong` byte HWM and the monitor snapshot fields |
| [Receive flow state](#receive-flow-state) | The receive-flow state type, setter, and monitor surface |
| [Error and validation policy](#error-and-validation-policy) | Validation timing and exception mapping |
| [Performance policy](#performance-policy) | Hot-path constraints |
| [Implementation checklist](#implementation-checklist) | What to confirm before declaring alignment, and required verification commands |
| [Actor and Spot Route results](#actor-and-spot-route-results) | The route-result record and Actor-directed send/request |

## Public contract source

- Public namespace: `Systems.Zlink`.
- Package identity: `Systems.Zlink`.
- Public contract: `bindings/dotnet/src/Zlink/Contracts/`.
- Runtime implementation: `bindings/dotnet/src/Zlink/Runtime/`.
- Internal implementation: P/Invoke declarations, `SafeHandle` or native handle ownership, callback trampolines, the request progress pump, native model converters, the socket kernel, option accessors, buffer codecs, validation helpers.
- Documentation's role: this README defines the library shape and review rules. `Contracts/` owns the exact public behavior surface.
- API reference comments: [`api-reference-comments.en.md`](api-reference-comments.en.md) defines the XML comment authoring and review criteria for `Contracts/`.

A runtime implementation file does not define a user-facing behavior that
can't be understood from `Contracts/` or a documented creation entry
point alone.

## Repository layout

Use the following paths consistently when changing the .NET binding.

- Public contract: `bindings/dotnet/src/Zlink/Contracts/`.
- Runtime implementation: `bindings/dotnet/src/Zlink/Runtime/`.
- Native bridge/artifacts: `bindings/dotnet/src/Zlink/Runtime/Native/`, `bindings/dotnet/native/`. Inside the NuGet package, these files are laid out under `runtimes/<rid>/native/`.
- Codec package: none provided. The .NET binding keeps only the raw `Message` and byte-payload API.
- Tests: `bindings/dotnet/tests/Zlink.Tests/`.
- Samples: `bindings/dotnet/samples/`.
- Perf: `bindings/dotnet/perf/`.

- `Contracts/`'s public signatures never include a P/Invoke declaration, `SafeHandle` detail, a native struct mirror used only for marshalling, or a request pump type.
- A concrete value type may use native-backed storage internally for ownership, but .NET does not expose or use a zero-copy send path that borrows a VM-managed buffer as a public or default behavior.
- Native bridge declarations and marshalling-only mirrors still live under `Runtime/Native/`.
- `Contracts/` and `Runtime/` are fixed repository folders.
- The `Systems.Zlink` namespace and the NuGet package surface are that contract projected onto .NET.
- A namespace segment named `Contracts` or `Runtime` is never exposed as the primary user-facing namespace.

The tree below is prescriptive about ownership and shows representative
files — it is not the complete file list.

- A file that defines public behavior lives under `Contracts/`.
- A file that calls native code, owns a handle, marshals a struct, or runs callback/request progress logic lives under `Runtime/`, and native bridge code lives under `Runtime/Native/`.

```text
bindings/dotnet/
+-- src/
|   +-- Zlink/
|   |   +-- Contracts/
|   |   |   +-- Core/
|   |   |   |   +-- Context.cs
|   |   |   |   +-- ContextOptions.cs
|   |   |   |   +-- RoutingId.cs
|   |   |   |   +-- Zlink.cs
|   |   |   +-- Messaging/
|   |   |   |   +-- Message.cs
|   |   |   |   +-- Received.cs
|   |   |   |   +-- TopicMessage.cs
|   |   |   |   +-- SubscriptionEvent.cs
|   |   |   |   +-- OperationContracts.cs
|   |   |   +-- Sockets/
|   |   |   |   +-- ISocket.cs
|   |   |   |   +-- MessageSocketContracts.cs
|   |   |   |   +-- RoutedSocketContracts.cs
|   |   |   |   +-- PubSubSocketContracts.cs
|   |   |   |   +-- IStreamSocket.cs
|   |   |   |   +-- SocketOptionFacades.cs
|   |   |   +-- Eventing/
|   |   |   |   +-- Monitor.cs
|   |   |   |   +-- Poller.cs
|   |   |   |   +-- PollEvent.cs
|   |   |   |   +-- Timer.cs
|   |   |   |   +-- ZlinkPoll.cs
|   |   |   +-- Service/
|   |   |   |   +-- SpotNode.cs
|   |   |   |   +-- Spot.cs
|   |   |   |   +-- Actor.cs
|   |   |   |   +-- SpotNodeModels.cs
|   |   |   +-- Errors/
|   |   |   |   +-- Errors.cs
|   |   +-- Runtime/
|   |   |   +-- Core/
|   |   |   +-- Handles/
|   |   |   +-- Messaging/
|   |   |   +-- Sockets/
|   |   |   +-- Eventing/
|   |   |   +-- Service/
|   |   |   +-- Errors/
|   |   |   +-- Buffers/
|   |   |   +-- Options/
|   |   |   +-- Native/
+-- tests/
+-- samples/
+-- perf/
+-- native/
+-- runtimes/
```

- The folder names `Contracts` and `Runtime` are a repository ownership boundary. They are not permission to expose `Systems.Zlink.Contracts` or `Systems.Zlink.Runtime` as a user-facing namespace.
- Public creation returns a public contract such as `IContext`, a socket interface, `ISpotNode`, `IPoller`, or `IZlinkTimer`, unless the public contract explicitly requires a concrete value type.
- Runtime classes such as `Context`, a socket class, `SpotNode`, `Poller`, or `Timer` are implementation owners, not the preferred consumer surface.

`Runtime/Buffers`, `Runtime/Handles`, and `Runtime/Options` are
implementation-support categories. These folders exist because real
native ownership, routing-id encoding, and option validation decisions
must be hidden inside the .NET binding. Another binding may use different
names for these support areas, but never moves that detail into a public
contract file.

## API change workflow

When mapping a new core feature:

1. Add the user-facing behavior to the appropriate `Contracts/` category.
2. Use a concrete DTO/value/record type unless the caller needs substitutable behavior.
3. Add or modify the `Runtime/` implementation without exposing a native bridge type.
4. Document a new creation entry point if the interface alone cannot construct the object.
5. Add tests against the public contract, not an `internal` member.
6. Update samples and perf only through the public contract and public factories.
7. Confirm a framework adapter does not reach the binding's private members via reflection or `InternalsVisibleTo`.

When refactoring existing .NET code:

1. Move user-facing declarations to their matching `Contracts/` category.
2. Move the native-backed implementation, handle ownership, request progress, marshalling, and option validation to `Runtime/`.
3. Keep P/Invoke declarations and native struct mirrors in `Runtime/Native/`.
4. Remove a duplicate public entry point that preserves only an old shape without reducing the caller's complexity.
5. Update samples, perf, and framework adapters only through the public contract and documented creation entry points.
6. Add or update tests through the public `Systems.Zlink` surface.

The refactor is considered complete once all of the following
.NET-specific shortcuts are gone.

- The public contract never mentions P/Invoke, `SafeHandle`, a native struct, a raw option id, callback userdata, request pump state, or a part-loop helper.
- A runtime class never introduces public behavior that can't be found in `Contracts/`.
- Framework adapters, samples, perf, and tests never use reflection, `NonPublic` lookup, or a private runtime shortcut.
- A compatibility wrapper is never kept only to preserve an old public shape.

## Library shape

The .NET binding uses a contract/runtime split.

- A behavior contract is a public `I*` interface in `Contracts/`. An operation builder contract may use a domain name such as `SendOperation` or `RequestOperation`, following the public shape that package has settled on.
- When a caller must create a resource through a public factory such as `Context`, `DealerSocket`, `RouterSocket`, `SpotNode`, `Poller`, or `Timer`, the native-backed implementation is an internal sealed class in `Runtime/`.
- A non-instantiable abstract base class may live in `Runtime/` purely as implementation support for those runtime implementation classes above. These are not creation entry points, and their public behavior must still be covered by a `Contracts/` interface or value type.
- DTO, value, result, option, enum, and exception types stay concrete types. They use ordinary .NET convention — `record`, `sealed class`, `readonly struct`, `enum`. An envelope that owns and must dispose a message part is a `sealed class`, not a `record`.
- An operation builder is an interface, so it can hide staged native request state and multipart accumulation.
- A public static facade, extension method, or builder convenience helper is part of the contract when the caller can call it directly. Even when the implementation delegates to runtime code, its definition lives under the owning `Contracts/` category.
- A native handle, request pump, callback bridge state, part-loop sequencing, or raw option id stays in `Runtime/` or an `internal` implementation type.
- A disposable native resource implements both `IDisposable` and `IAsyncDisposable`.

DTOs such as `Message`, `RoutingId`, `Received`, and `TopicMessage` are not
turned into interfaces just for symmetry. These are concrete domain values
whose ownership and allocation behavior are clear. `Received` is a
caller-provided, reusable recv storage, so it is created with
`Received.Create()`.

The standard interface classification other wrapper binding documents
follow is defined by the following .NET types.

- Core resource: `IContext`.
- Socket resource roles: `ISocket`, `IMessageSocket`, the routed socket contract, the pub/sub socket contract, and the pair/dealer/router/pub/sub/xpub/xsub/stream socket-family interfaces. A family interface exists only when that family has native-backed behavior.
- Eventing resource roles: the monitor socket contract, `IPoller`, the poll event source contract, `IZlinkTimer`. `ISpotNode`, `ISpot`, and, when an Actor handle is exposed, `IActor` or an equivalent actor resource contract.
- Operation builder roles: send, routed send, request, reply, publish, channel send/request, SPOT send/request/reply, actor create, actor join, actor join reply operations.
- Callback roles: stream packet handler, monitor handler, poll handler, SPOT dispatch handler, route handler, admission handler, reply callback.

### RoutingId string and binary helpers

`RoutingId` stays a binary-safe value type. The public .NET helpers have
the following meaning.

- `RoutingId.From(string value)` encodes a user routing id string as UTF-8.
- `RoutingId.From(byte[] value)` and `RoutingId.From(ReadOnlySpan<byte> value)` preserve the routing id's raw bytes as-is.
- `RoutingId.FromHex(value)` restores the bytes that `ToHex()` printed.
- `RoutingId.From(uint value)` records a 4-byte big-endian `uint32` routing id.
- `RoutingId.From(Guid value)` records a 16-byte UUID routing id.
- `ToString()` is for display: printable UTF-8 text, then `uint32`, then UUID, and a `hex:`-prefixed raw hex when no clearer representation applies.

For a durable raw-byte round trip, use `ToHex()` / `FromHex(value)`.

`RoutingId` caching is purely an internal optimization. The binding may
cache a hash or a short-lived receive-path value, but equality and public
behavior are defined only by the immutable byte value.

## Contract / Runtime placement rules

- A public interface, a concrete DTO/value type, an enum, and the public exception domain live in `Contracts/`.
- A public static facade, extension method, module-style helper, or builder convenience helper lives in `Contracts/`.
- The runtime implementation, the socket kernel, the request pump, callback bridge state, and lifecycle owners live in `Runtime/`.
- P/Invoke declarations, `SafeHandle` implementations, native struct mirrors, marshalling helpers, and platform loading code live in `Runtime/Native/`.
- `Contracts/`'s public signatures never mention a `Runtime/Native/` type.
- Even when a runtime class is intentionally exposed for direct construction, its public behavior is still described by `Contracts/`. This is the exception, not the default shape.

## Standard interface rules

- Data-plane `Recv`, routed recv, `Subscribe`, and subscription event receive fill a caller-provided `Received`, `TopicMessage`, or `SubscriptionEvent` instance and return `bool`.
- A .NET caller creates a reusable receive storage with `Received.Create()`. `Received` has no public constructor.
- `Send`, routed send, `Publish`, `Request`, `Reply`, SPOT operations, and Actor location/session operations return a fluent operation builder.
- The terminal for PUB/XPUB `Publish(topic)` is the synchronous
  `PublishSubmitOperation.Submit() -> void`. Default PUB semantics are lossy, so
  the publisher never waits at the high-water mark. Under `NODROP` a full
  subscriber surfaces on the spot as `ZlinkSubmitException`
  (`Result == Backpressured`), and the retry policy belongs to the application.
  `TryPublish(topic)` is the separate surface that observes the same
  back-pressure as `false` instead of an exception.
- A back-pressure-cleared callback (`OnSendReady`) is not part of the public
  contract: the `send_ready` readiness-hint semantics were withdrawn in Core
  0.13.0.
- A builder's start method takes only a target identity, topic, channel, routing id, or request sequence. Payload, flag, timeout, callback, and async submit choices are handled at the builder stage.
- A reply builder has no send-flag stage. Since the core reply function takes no send-flag argument, the .NET binding does not expose a no-op `Flags(...)` as part of the public contract.
- The terminal for a raw ROUTER/`Received` reply is the synchronous one-shot
  `ReplySubmitOperation.Submit() -> void`. It submits a terminal reply or error
  reply to the HWM-free completion lane with one native call. HWM backpressure
  is not a reply result; `NotConnected`, `Terminated`, `InvalidArgument`, and
  other non-HWM submit failures immediately throw `ZlinkSubmitException`.
- No single-payload shortcut overload is added under the same name as an operation's start method. `Send(Message)`, `Send(RoutingId, Message)`, `Publish(string, Message)`, `SendToChannel(string, Message)`, `SendToSpot(..., Message)` are not public contract members. A caller uses the builder terminal for that role; `Send(...).Message(message).Async()` is canonical for a DEALER/ROUTER routed send.
- A multipart payload accumulates via repeated `Message(...)` calls. A `Messages(...)`-style convenience method is allowed, but since it is a public builder contract member, it lives in `Contracts/`.
- `IDealerSocket` does not expose protocol envelope helpers such as `RequestFrame(...)` or `Reply(requestToken, parts)`. A dealer can start a request with `Request()`, but has no API-level peer routing id, so it cannot reply to an arbitrary token. Reply starts from a received request context, or from an explicit router/SPOT reply surface when the target context requires it.
- A message payload factory uses `Message.From(...)` overloads. A source-type suffix such as `FromBytes`, or a value-style factory such as `Of`, is not part of the public contract.
- No operation-start method family such as `SendNoWait`, `PublishWithFlags`,
  `RequestAsync` is added. Keep one operation name, and let the builder absorb
  variants. An awaitable terminal on an HWM-managed DEALER/ROUTER routed
  send/request builder is unified as `Async(...)`.

### DEALER/ROUTER routed asynchronous terminals

- `IDealerSocket.Send()` and `IRouterSocket.Send(RoutingId)` return
  `RoutedSendOperation`. The only terminal on `RoutedSendSubmitOperation` is
  `Task Async(CancellationToken)`. Both sockets inherit only
  `IReceivingMessageSocket` and connect roles, which contain no synchronous
  `SendOperation`. Conversion to a base interface therefore cannot recover
  `Flags(...)` or `Submit()`.
- The only terminal on the `RequestSubmitOperation` returned from
  `IDealerSocket.Request()` or `IRouterSocket.Request(RoutingId)` is
  `Task<IReadOnlyList<Message>> Async(CancellationToken)`. These routed builders
  do not add a blocking submit, polling result, `Flags(...)`, or callback
  `Submit(...)` terminal.
- Multipart payloads accumulate through repeated `Message(...)` calls or
  `Messages(...)`, followed by one `Async(...)`. `Async(...)` transfers payload
  ownership to the operation and returns a Task without blocking the caller
  thread.
- The initially selected target remains fixed until the operation terminates.
  HWM waiting never reroutes the operation to another target.
- While HWM credit is unavailable, the operation occupies neither the caller
  thread nor a worker thread, and it holds no socket-wide lock that prevents
  another send from progressing.
- HWM waiting on one target does not delay admission for another target. This
  independence is not a public strict-FIFO guarantee for a routing id.
- Request passes builder `Timeout(...)` to Core as the per-request deadline, and
  Core reports expiry as `ZLINK_REQUEST_TIMED_OUT`. The routed send admission
  deadline is the Core-side per-operation option. Cancellation, close,
  disconnect, target loss, timeout, and a fast reply may race, but only one Task
  terminal wins.
- Cancellation completes the Task as canceled, so awaiting it throws
  `OperationCanceledException`. If routed send is not admitted by the Core
  deadline, it faults with `ZlinkSubmitException`
  (`Result == Backpressured`); if request does not finish by `Timeout(...)`, it
  faults with `ZlinkRequestException` (`Result == TimedOut`).
- Not awaiting the returned Task, or discarding its reference, does not cancel
  the operation. Payload ownership and terminal cleanup remain with the
  operation until one of the preceding terminal conditions completes it.
- The public contract has no separate queue capacity or queue-full result and
  requires no Framework retry or polling.
- Unrelated common synchronous data-plane builders for roles such as PAIR and
  STREAM remain separate contracts; they are not canonical terminals for the
  DEALER/ROUTER HWM-managed routed path.
- Core's send-completion notification drives routed send completion. The socket
  runtime installs `zlink_send_complete_handler` once, on the first `Async(...)`,
  and every operation then hands one complete record to `zlink_send_async`. The
  binding owns no park queue, no readiness retry pump, no deadline timer and no
  dispatcher thread — the `send_ready` readiness-hint semantics are withdrawn.
- The completion callback only delivers completion. It never calls send,
  publish, or request on any socket; Core refuses such a call with `EDEADLK`.
- The completion delegate instance handed to Core must outlive the socket. The
  runtime roots it through both a field and an explicit `GCHandle`, and releases
  it only after the native socket is closed. Collecting it early would leave
  Core calling a freed reverse-P/Invoke stub.
- Operation state is kept alive by the `GCHandle` that travels through Core as
  the operation `userdata`; the exactly-once completion frees that handle. A
  submit that is not `SUBMIT_OK` runs no completion, so payload ownership
  returns to the caller immediately.
- A record admitted immediately runs its completion inline inside
  `zlink_send_async`, so the caller never suspends on that path.
- `CancellationToken` maps to `zlink_send_async_cancel`. A cancelled operation
  still completes exactly once (`TERMINAL` / `ECANCELED`), and an operation
  whose admission is already committed is not cancelled and completes as
  `ADMITTED`.
- The per-operation deadline is a Core-side option
  (`zlink_send_async_options_t.timeout_ms`). The binding owns no deadline timer.
- PUB/XPUB `publish` is not part of this surface: Core's `zlink_send_async`
  returns `ENOTSUP` for PUB/XPUB, and the publish terminal is the synchronous
  `Submit()`.

## Contract folder layout

`Contracts/` must be readable as a public API map.

- `Core/`: context, context option, routing id, utility resource contracts.
- `Messaging/`: message, received metadata, topic message, subscription event, common send/request/reply operation contracts, message-domain convenience helpers.
- `Sockets/`: socket operation contracts, socket capability interfaces, typed option facades.
- `Eventing/`: monitor, monitor snapshot/event, poller, timer, poll event contracts. A static poll helper, when public, also belongs here.
- `Service/`: SPOT node, SPOT handle, the topology model, actor ref, actor lifecycle, service-only operation builders.
- `Errors/`: the exception hierarchy and error-domain mapping.

Files within each category are split by user-facing concept, not
implementation order.

- Common messaging operations split into send, request, and reply; the service topology model splits into the SPOT node model and shared topology enums.
- A request result belongs to the messaging request contract, not a socket enum file.
- A received message kind stays with the received message metadata.
- SPOT node mode, socket snapshot, Spot snapshot, and actor snapshot belong to the SPOT node model.

SPOT stays a single handle contract, `ISpot`. It is not split into
per-role interfaces unless the caller genuinely needs to receive those
roles separately.

- `ISpotNode` may split node configuration, peer connection, Spot creation, Actor operations, and topology lookup roles into separate interfaces that compose. Even so, the default creation path and the user-facing return type remain `ISpotNode`, and a role interface must never expose a runtime implementation type.
- SPOT callback registration uses a named callback delegate, so the public signature describes the callback's meaning without adding a wrapper context object.
- A registration method uses the `Set...Handler` name because it stores or replaces the current handler. An `On...` name is reserved only for a method invoked when the event occurs.
- Since these delegates are used only in the SPOT handle contract, they are declared next to `ISpot`.
- A lifecycle data type lives with the actor model. A lifecycle event envelope that owns a message part is a sealed class, not a cloneable record.
- The Actor operation contract splits into join, management, and session binding.

If a user or a framework adapter needs a public API, that API must be
discoverable in this folder without reading P/Invoke or runtime bridge
code.

## Runtime folder layout

`Runtime/` follows the same standard map, but contains only
implementation.

- `Core/`: context lifecycle, counter/stopwatch/thread helpers, runtime version/capability lookup.
- `Handles/`: native resource ownership, close state, lifetime checks, reference tracking.
- `Messaging/`: multipart message materialization, request/reply progress, request state, received handlers, topic encoding.
- `Sockets/`: the socket base class, the socket kernel, socket implementations, callback adapters, option accessors, receive helpers, operation implementation classes.
- `Eventing/`: poller, timer, monitor state, callback delivery, event materialization helpers.
- `Service/`: SPOT node, Spot, Actor, topology converters, service option support, service operation implementations.
- `Errors/`: boundary validation, native result mapping, errno conversion.
- `Buffers/`: the routing-id codec, payload buffer ownership, the copy/borrow policy, snapshot buffer helpers.
- `Options/`: context/socket option constants, validation, runtime option conversion.
- `Native/`: P/Invoke declarations, platform loading, native type mirrors, marshalling helpers.

Runtime code may depend on public contract types. A contract file may
internally delegate to runtime code to wire up a public factory/static
facade, but a public signature must never expose runtime implementation
detail.

## Creation entry points

An interface defines behavior; creation is provided by a public factory.

- `Zlink.CreateContext()` creates the runtime context implementation.
- `Zlink.CreateAtomicCounter()`, `CreateStopwatch()`, `CreateThread(...)` create utility resources through the public contract.
- `IContext.CreatePairSocket()`, `CreateDealerSocket()`, `CreateRouterSocket()`, `CreatePubSocket()`, `CreateSubSocket()`, `CreateXPubSocket()`, `CreateXSubSocket()`, `CreateStreamSocket()` create runtime socket implementations.
- `IContext.CreateSpotNode()` and `CreateSpotNode(SpotNodeMode)` create the service-layer implementation.
- A `Spot` handle is obtained via `ISpotNode.CreateSpot()`, `ISpotNode.EntrySpot()`, `ISpotNode.GetOrCreateSpot(...)`, or `ISpotNode.SpotLookup(...)`. Directly constructing a `Spot` is not public. `GetOrCreateSpot(...)` maps directly to `zlink_spot_node_spot_get_or_new(...)`, and is never implemented by combining lookup and create in managed code.
- An `Actor` handle is created with `ISpotNode.CreateActor(...)`. Directly constructing an Actor is not public.
- `Zlink.CreatePoller()`, `Zlink.CreateTimer()`, `Zlink.CreateTimer(ISpot)` create eventing resources.
- `Zlink.Version()`, `Zlink.Has(...)`, `Zlink.Strerror(...)`, `Zlink.Proxy(...)`, `Zlink.ProxySteerable(...)`, `Zlink.Sleep(...)`, `Zlink.MultipartClose(...)`, `ZlinkPoll.Poll(...)` are public static facades. Even though their native calls remain in `Runtime/`, their callable behavior is part of the contract surface.

A factory's return type favors the public contract wherever the caller
does not need the concrete runtime type.

## Required feature coverage

The .NET public contract covers every stable, user-facing core feature.
The shape may be narrower or more idiomatic than C, but the meaning stays
the same.

- Context lifecycle, options, shutdown, auto-HWM recalculation, version, capability helpers, strerror.
- Message ownership, multipart payload, routing id, received metadata, topic message, subscription event.
- pair, dealer, router, pub, sub, xpub, xsub, stream sockets.
- Common options, typed socket options, TLS, bind/connect/disconnect, routing id, channel name, request/reply, publish/subscribe, callback surfaces.
- socket monitor, monitor event/snapshot, poller, poll event, timer, SPOT timer integration.
- SPOT node, SPOT handle, topology snapshot, actor ref, actor operations, actor lifecycle, stream actor binding.
- Typed exceptions for submit, request, recv, handler, close, bind, connect, config failures.

A native helper function that exists only to support the part loop,
callback userdata, interop marshalling, or request progress stays
internal.

A `Message` receive wrapper may return to a bounded thread-local pool only
after its owner `Received` or `TopicMessage` removes part references and
completes `Dispose()`. The `Message` reference is invalid when deterministic
cleanup returns. The caller must not use it again, including repeated
`Dispose()`, payload access, or object-identity dictionary lookup. A wrapper
that has not been returned and a caller-created owned `Message` are not reused
for another ownership.

## Receive and Subscribe shape

.NET's recv-family data-plane API uses caller-provided output storage for
allocation-free draining.

- Message/routed receive fills a caller-provided `Received` object, created with `Received.Create()`, and returns `bool`.
- Raw `SUB`/`XSUB` and SPOT subscribe fill a caller-provided `TopicMessage` or `SubscriptionEvent` object and return `bool`.
- `TopicMessage.ReleaseForReuse()` releases the current parts and metadata while retaining the
  internal topic receive buffers for a later `Subscribe` call. It is valid only while the
  `TopicMessage` is open; after terminal `Dispose()` it throws `ObjectDisposedException` and
  does not reopen the object.
- Ordinary `Recv(Received, ...)` and `Subscribe(TopicMessage, ...)` keep their existing behavior
  of returning Core queue credit at dequeue.
- A Framework backend explicitly selects `RecvRetained(Received, ...)` or
  `SubscribeRetained(TopicMessage, ...)`. These entry points preserve existing framing and
  metadata while internally adopting one Core retained-credit lease for each caller-visible
  physical part. `Received` releases its current parts and all leases exactly once on
  `Dispose()` or when the same storage starts another receive. `TopicMessage` does the same on
  `Dispose()`, `ReleaseForReuse()`, or when the same storage starts another subscribe. An
  internal finalizer is only a leak-prevention fallback; normal paths perform deterministic
  cleanup first. A retained payload copy does not own the lease.
- Retained receive is an internal lifetime behavior of the aggregate `Received` and
  `TopicMessage` paths. Part-level primitives and ordinary aggregate receive keep their existing
  behavior; the binding adds neither a public lease handle nor separate application capacity.
  Dealer message type and request sequence, Router source RID and request sequence, and SUB topic
  and source RID remain the existing typed receive results.
- `false` means no data only for a non-blocking receive using `RecvFlags.DontWait`.
- A real receive failure (one that is not simply no-data) throws `ZlinkRecvException`.
- A control-plane API such as monitor recv or timer recv may keep a nullable return form when no-data is a natural value shape.
- A service control/admission API such as `RecvActorJoin(...)` may also keep a nullable return form. These are not data-plane drain APIs, but they still distinguish no-data from a real receive failure (one that is not simply no-data).

SPOT's `SubscribeReadable` and `RoutedReadable` dispatch events are
readiness notifications. The caller drains the matching receive API until
no-data is reported.

## Service and SPOT shape

SPOT is a service-layer API — it is never a leak of the raw socket.

- `ISpotNode` owns node lifecycle, route identity, peer connections, route bridge/channel coordination, external pub-ingress attachment, topology snapshot, spot creation, and actor creation.
- `ISpot` owns SPOT topic publish/subscribe, routed send/request/reply, routed receive, dispatch events, actor join receive/reply, and actor lifecycle callbacks.
- `Spot.Publish(topic)` enters the owning node's SPOT topic plane. It never exposes or selects a raw `PUB` socket.
- `Spot.Publish(topic)` keeps its short publish name because the caller already holds a publishable `Spot`. The binding contract does not rename it to `PublishSpot` or `PublishToTopic`.
- A channel-targeted SPOT operation uses `SendToChannel(...)` and `RequestToChannel(...)`, so the destination-bearing send/request names stay aligned with `SendToSpot(...)`, `RequestToSpot(...)`, `RequestToRouter(...)`.
- Actor location and stream session binding are independent of each other. An actor joining a user Spot does not require a bound stream session.

## Byte HWM and monitoring ABI v4

- HWM is a limit on Core-computed accounted bytes, not a message count on the queue.
- The public type is `ulong`, which does not shrink Core's `uint64_t` range.
- `0` means unlimited, and the manual default is `4_096_000 bytes`.
- The binding calls Core with an exact 8-byte value.
- No previous `int` overload, alias, or count-unit adapter is provided.

```csharp
public interface IContextOptions
{
    ulong CoreHwmMemoryLimitBytes { get; set; }
    ulong CoreHwmBudgetBytes { get; set; }
    CoreHwmProfile CoreHwmProfile { get; set; }
}

public interface IContext
{
    CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot();
    void ResetCoreHwmBudgetMetrics();
}

public partial class CommonSocketOptions
{
    public ulong SendHighWaterMark { get; set; }    // Outbound accounted-byte limit.
    public ulong ReceiveHighWaterMark { get; set; } // Inbound accounted-byte limit.
}
```

Input precedence is manual Core budget, explicit memory limit, the available-
memory limit reported by .NET GC, then Core fallback. Setting either of the
first two values disables automatic GC-hint detection. The binding does not
combine the hint with Core's hard limit. If an explicit input exceeds a finite
hard limit Core detected, the binding preserves the existing configuration
exception corresponding to `EINVAL` and does not clamp the value.

Core applies the profile ratio to the memory limit exactly once, or uses an
explicit Core budget unchanged, then calculates planned byte HWM per physical directional queue. A
direction on which the caller sets `SendHighWaterMark` or
`ReceiveHighWaterMark` becomes a manual override and is not changed by later
automatic HWM recalculation.

The .NET binding does not recount queued messages or payloads. When the actual
accounted bytes in a Core pipe reach the applied HWM, the native submit result
reports backpressure and the .NET operation preserves it through the existing
result and timeout contract. `0UL` means unlimited.

`MonitorOpen(events, monitorHwmBytes)` accepts an exact `ulong` byte value for
the monitor queue. `0UL` selects the Core monitor default; a positive value is
forwarded unchanged. There is no message-count overload or alias.

- `MonitorStatus` provides the same fields as the native `zlink_monitor_status_t` ABI version 4.
- Planned, applied, and deferred HWM, and in-flight usage, are all `ulong` byte values.
- A deferred value is valid only when the matching `AutoHwmDeferredSendHighWaterMarkValid` or `AutoHwmDeferredReceiveHighWaterMarkValid` is `true`.
- A pending-message value stays a count diagnostic value, `SndPendingMsgs` and `RcvPendingMsgs`, and never shares a name with a byte field.
- Pending bytes are exposed separately as `SndPendingBytes` and `RcvPendingBytes`.
- If a snapshot's `AbiVersion` is not `4`, or its `StructSize` differs from the binding layout, it throws `NotSupportedException`. An older monitoring layout is not accepted.

`CoreHwmBudgetSnapshot` projects ABI version/size, configured/runtime/resolved
memory limits, configured/effective budgets, planned/applied/manual-reserved
HWM, Core-queue/application/current/peak/provisional accounted bytes,
completion current/peak/pending and total-messaging values, monitor/instance
aggregates, application/completion queue counts,
`OutstandingApplicationLeaseCount`, `RetiredQueueCount`,
`DeferredOriginCreditBytes`, oversize/blocked/aggregate flags,
`BudgetGeneration`, and `MeasurementEpoch` without unit conversion. Reset
preserves current, pending, queue-count, and those three owner-lifecycle gauges,
rebases both peaks to current, clears epoch counters, and increments
`MeasurementEpoch`. A budget snapshot ABI version/size mismatch throws
`NotSupportedException`.

Request/reply APIs take no HWM value as an argument. While `Async(...)` waits
for HWM credit on the selected exact target, it preserves the request's
original deadline. The caller implements neither retry nor polling.

## Receive flow state

The binding exposes the Core receive-flow state as the `ReceiveFlowState`
enum with `Running = 0` and `Paused = 1`. `ISocket.SetReceiveFlowState(ReceiveFlowState)`
sets it. The method returns `void` and follows the .NET error policy: a
non-zero native `zlink_config_result_t` is thrown as a `ZlinkConfigException`
whose `ErrorCode` is the matching `ConfigResult`, so a socket without a
completion lane raises `ZlinkConfigException` with `ConfigResult.NotSupported`.
Setting the state the socket already holds returns normally.

The observation surface follows the C contract, so the constant and metric
names are fixed by the C layer: the monitor events `SEND_FLOW_PAUSED`,
`SEND_FLOW_RESUMED`, and `FLOW_STATE_STALE` (`1 << 16`, `1 << 17`, `1 << 18`,
with the full mask `0x7FFFF`), the event flags `SEND_FLOW_WRITABLE` (`1 << 1`),
`FLOW_STATE_STALE_GENERATION` (`1 << 2`), and `FLOW_STATE_STALE_EPOCH`
(`1 << 3`), the status detail bit `FLOW_STATE` (`1 << 5`), and the five status
fields `flow_paused_connections`, `flow_pause_applied_total`,
`flow_resume_applied_total`, `flow_state_stale_total`, and
`flow_pause_duration_ms`, projected with this language's naming convention.

Flow-state frames stay inside Core. The binding calls the setter, reads the
monitor events and the snapshot fields, and never encodes, decodes, sends, or
receives a flow-state frame itself.

## Error and validation policy

- A fixed-size native boundary value is validated before calling core.
- An invalid routing id, actor id, endpoint, channel name, or topic throws a .NET argument/config exception before truncation would occur.
- submit, request, recv, handler, close, bind, connect, and config errors map to a typed zlink exception.
- A typed zlink exception's public constructor must never accept the success value `Ok`. The `Ok` enum member stays as a native result mirror, but the public constructor accepts only a failure code. A constructor that also accepts a native errno is for internal runtime conversion and is not public surface.
- No-data and transient backpressure are never reported as an ordinary exception.
- The public API never requires a caller to inspect a native errno directly.

## Performance policy

- The hot path never uses reflection, dynamic invocation, repeated boxing, avoidable allocation, avoidable buffer copies, hidden sleeps, busy waits, thread joins, or broad locks.
- Native interop creates `Message`, `Received`, and `TopicMessage` values managed directly from the core part substrate. A public, caller-owned `Received` buffer is created with `Received.Create()`.
- A caller that drains repeated publishes may call `TopicMessage.ReleaseForReuse()` after the
  current consumers finish. This avoids reallocating the topic receive buffers without changing
  the ownership of the current message parts.
- Request progress is shared per handle wherever possible. It does not create a new polling thread or timer per request.
- Perf, samples, and framework adapters use only the public contract and creation entry points.

## Implementation checklist

Before declaring the .NET binding aligned:

- `Contracts/` exposes every public behavior a user or framework adapter needs.
- `Runtime/` implements that contract without adding a hidden, user-facing API.
- Concrete value types stay concrete.
- The default creation path is documented and tested.
- A public static facade, extension helper, or builder convenience method is discoverable in `Contracts/`.
- The recv/sub API uses the caller-provided storage shape.
- Any exception where service control/admission receive differs from the data plane's caller-provided storage is documented.
- Perf semantics match `bindings/c/perf`. A private runtime shortcut never changes the meaning of a measurement.
- `Contracts/`'s public signatures never expose `Runtime/Native/`, a raw handle, a native struct mirror, a request-progress type, or a runtime implementation class. An internal delegation from a static facade to runtime code is allowed.
- A runtime class never becomes a second contract surface.
- A framework adapter calls the public binding API directly.
- No old alias, duplicate operation-start name, or deprecated wrapper preserved only for compatibility remains.

Required verification after a .NET binding change. Run the following
commands from `bindings/dotnet/`.

- Run `dotnet test Zlink.sln`, or the repository's current .NET binding test solution.
- Run `./tests/run_tests.sh`.
- Run `./samples/run_samples.sh` when a public example or a generation path changed.
- Run `./perf/run_benchmarks.sh` and `./perf/run_benchmarks_multi.sh` as a smoke gate when hot-path, receive, send, request, poller, timer, or service behavior changed.
- Search framework adapters, samples, perf, and tests for reflection, `NonPublic`, `InternalsVisibleTo`, `Runtime.Native`, raw handle use, or direct request-pump access.

## Actor and Spot Route results

`.NET` exposes route lookup results as a public contract record.

- `ActorRoute` preserves the resolved `ActorRef`, `Actor.NodeRid`, `CurrentSpotRid`, `CurrentSpotKind`.
- `SpotRoute` preserves `SpotRid`, `OwnerNodeRid`, `SpotKind`.
- `SpotKind` distinguishes an Entry Spot from a user Spot. An invalid kind is not a successful route result.
- `SpotNodeSpotEntry` and `SpotNodeActorEntry` expose the same Spot kind/current Spot fields as the core snapshot.

- The binding exposes `ISpotNode.SendToActor(ActorRef)` and `ISpotNode.RequestToActor(ActorRef)`, which take a resolved Actor ref.
- `SendToActor`, once submit succeeds, transfers ownership of one or more message parts, and completes once the Actor owner's mailbox takes them over.
- `RequestToActor`, once submit succeeds, transfers ownership of the request part and delivers the reply part the Actor handler produced, as a task or a callback.
- The binding must not resurrect the removed Discovery route table or resolver API as a compatibility helper.
