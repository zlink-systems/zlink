---
title: "Java Binding Implementation Blueprint"
---

<!-- bindings-nav:start -->
[Spec index](../README.en.md) | [Previous: C++](../cpp/README.en.md) | [Next: Node.js](../node/README.en.md)
<!-- bindings-nav:end -->

# Java Binding Implementation Blueprint

> **What this chapter defines** — the Java binding's `contracts`/`runtime` structure and JPMS export
> boundary.

This document defines the Java binding's public contract and ownership structure. It is not an
exhaustive method reference. `bindings/java/src/main/java/systems/zlink/contracts/` owns the exact
public member list.

The Java binding uses the same architecture map as the .NET binding:

- public resource behavior is expressed as contract interfaces;
- native-backed runtime implementations live under runtime packages;
- creation flows through public factory entrypoints;
- DTO, value, record, enum, result, and exception types stay concrete;
- runtime/native details do not appear in public contract signatures;
- tests, samples, perf, and applications import only public contract packages.

The Java public contract classification follows the
[.NET binding blueprint](../dotnet/README.en.md) as the baseline. Java does not have to
copy every C# file literally when Java's public type rules make that awkward,
but it must preserve the same category ownership, resource boundary, and
operation/model grouping.

| Section | Covers |
|---|---|
| [Source Of Truth](#source-of-truth) | The semantic source of truth and the Java repository ownership boundary |
| [Architecture Requirements](#architecture-requirements) | Required contract/runtime ownership boundaries |
| [Architecture Map](#architecture-map) | The `contracts`/`internal`/`runtime` package tree |
| [Public Contract Categories](#public-contract-categories) | A table of contract/runtime packages to purpose |
| [Native Wait Boundary](#native-wait-boundary) | The boundary between blocking recv and poller-based receive |
| [Proposed Repository Layout](#proposed-repository-layout) | The full Gradle project directory tree |
| [Contract Interface Rule](#contract-interface-rule) | Types that stay interfaces and types that stay concrete |
| [Factory Entry Points](#factory-entry-points) | Root/context/service factory methods |
| [Contract File Requirements](#contract-file-requirements) | What a contract file may and may not import |
| [Runtime Implementation Requirements](#runtime-implementation-requirements) | The implementation detail runtime owns |
| [Socket Contract Shape](#socket-contract-shape) | Common and per-type socket behavior |
| [Operation Builder Shape](#operation-builder-shape) | Builder start methods and terminal methods |
| [Messaging Values](#messaging-values) | The `Message`/`Received`/`TopicMessage`/`SubscriptionEvent` contract |
| [Receive And Subscribe Shape](#receive-and-subscribe-shape) | Caller-provided storage, no-data, and the Core-HWM ownership boundary |
| [Handler Registration Naming](#handler-registration-naming) | The `set...Handler` naming rule |
| [Byte HWM And Monitoring ABI v4](#byte-hwm-and-monitoring-abi-v4) | Non-negative `long` HWM and monitor snapshot fields |
| [Receive flow state](#receive-flow-state) | The receive-flow state type, setter, and monitor surface |
| [Error And Result Policy](#error-and-result-policy) | Typed exceptions and validation timing |
| [Spot And Actor Contract Shape](#spot-and-actor-contract-shape) | `SpotNode`/`Spot` responsibilities and route results |
| [Spot Get-Or-Create](#spot-get-or-create) | The `getOrCreateSpot` contract |
| [Performance Policy](#performance-policy) | Hot-path constraints |
| [Architecture requirements](#architecture-requirements-1) | Contract/runtime boundary requirements |
| [Implementation Checklist](#implementation-checklist) | Checks before declaring alignment |
| [Verification](#verification) | Required verification commands and structural searches |

## Source Of Truth

The semantic source of truth is `core/include/zlink.h`. The shared binding
policy is `doc/spec/bindings/README.md`. The .NET projection is the
[.NET binding blueprint](../dotnet/README.en.md), and Java follows that design
while using Java package names and Java naming conventions.

The Java repository ownership boundaries are:

- Public contract source:
  `bindings/java/src/main/java/systems/zlink/contracts/`.
- Native-backed runtime implementation:
  `bindings/java/src/main/java/systems/zlink/runtime/`.
- Native bridge and Panama/JNI downcalls:
  `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/`.
- Native artifacts and resources:
  `bindings/java/src/main/resources/native/` and `bindings/java/native/`.
- Tests: `bindings/java/src/test/` and `bindings/java/tests/`.
- Samples: `bindings/java/samples/`.
- Perf: `bindings/java/perf/`.

JPMS exports must include only documented packages under
`systems.zlink.contracts.*`. Packages under `systems.zlink.runtime.*`,
including `systems.zlink.runtime.nativeapi`, are implementation packages and
must not be exported.

## Architecture Requirements

The Java package structure meets these conditions.

- a native-backed resource is typed by a public contract interface;
- a native-backed implementation lives under `systems.zlink.runtime.*`;
- a factory returns a contract type and hides the runtime class;
- a public contract file does not import `systems.zlink.runtime.*`;
- samples, perf runners, and tests do not import runtime packages;
- native handles, raw part loops, completion-drain state, and native struct mirrors remain outside public
  contract source.

The contract/runtime split applies at resource boundaries as well as helper boundaries.

## Architecture Map

Java uses lower-case package names for the same conceptual map used by .NET.
The package names are Java-specific, but the ownership rules are the same.

```text
bindings/java/src/main/java/systems/zlink/
+-- contracts/
|   +-- core/
|   +-- messaging/
|   +-- sockets/
|   +-- eventing/
|   +-- service/
|   |   +-- spot/
|   +-- errors/
+-- internal/
+-- runtime/
|   +-- core/
|   +-- messaging/
|   +-- sockets/
|   +-- eventing/
|   +-- service/
|   |   +-- spot/
|   +-- errors/
|   +-- nativeapi/
```

`contracts` is the public API map. A reviewer should be able to understand all
user-observable behavior by reading this tree and the public factories.

`internal` is the non-exported bridge map. It exists only for code that must
connect contract-owned state to runtime implementations without making those
hooks application-facing API.

`runtime` is the implementation map. It mirrors the Java target classification:
`core`, `messaging`, `sockets`, `eventing`, `service`, `errors`, and Java's
`nativeapi` equivalent of `.NET` `Runtime/Native`. It owns native handles,
downcalls, marshalling, completion-drain state, socket
kernels, service kernels, option mapping, and lifecycle details. Runtime
support code such as handle lifetime, buffer conversion, and option mapping is
kept under the owning runtime category instead of introducing extra public
package categories.

The two trees do not need a strict one-file-to-one-file mapping. They do need
clear ownership. For every native-backed resource, there must be a public
contract owner and a runtime implementation owner.

## Public Contract Categories

The Java contract categories are normative.

| Package | Purpose |
|---------|---------|
| `systems.zlink.contracts.core` | Library entrypoint, context resource contract, routing ids, version/capability helpers, process-level helpers. |
| `systems.zlink.contracts.messaging` | Message values, received envelopes, topic messages, subscription events, payload ownership, common message metadata. |
| `systems.zlink.contracts.sockets` | Socket resource contracts, socket operation builders, socket options, send/recv/request/reply/publish surfaces. |
| `systems.zlink.contracts.eventing` | Poller, poll events, monitor socket, monitor snapshots, timer resource contracts. |
| `systems.zlink.contracts.service.spot` | SpotNode, Spot, Actor, route/admission handlers, actor lifecycle, and service operation builders. |
| `systems.zlink.contracts.errors` | Public exception and typed error/result domains. |

Runtime packages use the same .NET-standard classification with Java package
names:

| Package | Purpose |
|---------|---------|
| `systems.zlink.runtime.core` | Context implementation, context option application, runtime version/capability calls. |
| `systems.zlink.runtime.messaging` | Message materialization, multipart progress, request execution, and completion-registry integration. |
| `systems.zlink.runtime.sockets` | Socket kernels, socket family implementations, poller drain, and socket operation execution. |
| `systems.zlink.runtime.eventing` | Monitor, poller, poll event, timer, and dispatch loop implementations. |
| `systems.zlink.runtime.service.*` | SpotNode, Spot, Actor, topology, and service operation implementations. |
| `systems.zlink.runtime.errors` | Native errno/result conversion into public exception/result domains. |
| `systems.zlink.runtime.nativeapi` | JNI/Panama declarations, ABI mirrors, symbol loading, and native artifact lookup. |

Enum, flag, and result types live with the concept that gives them meaning.
Do not create syntax-only public Java packages such as `enums` or `callbacks`.
Physical source folders such as `SocketEnums/` are allowed only as file
classification groups when the Java `package` declaration remains the owning
contract package.

## Native Wait Boundary

The Java binding separates the low-level socket recv API from the poller-based
receive boundary.

- `socket.recv(received, RecvFlags.NONE)` is a native blocking recv. It may be
  used directly from a small number of dedicated threads or in simple tests.
- A framework or a runtime that handles many client sessions does not put a
  blocking recv directly on the handler execution thread. The runtime waits for
  readiness with `Poller` and then performs a `RecvFlags.DONT_WAIT` recv on the
  ready socket.
- Application handlers run behind the framework-configured handler executor, not
  on the native wait thread. If virtual threads are used, they are used at this
  handler-executor boundary.
- No separate public dispatcher API is provided. The existing `Poller`, socket
  `recv(..., DONT_WAIT)`, and the framework's internal receive loop are
  sufficient; framework execution policy is not mixed into the public bindings
  contract.

## Proposed Repository Layout

This is the review target for the Java binding repository layout. It keeps the
same public contract classification as `.NET`, while using Java package names
and Java file rules. The sample and perf directories keep their Gradle project
shape; the classification rule applies to the source package trees inside them.

```text
bindings/java/
+-- build.gradle.kts
+-- settings.gradle.kts
+-- gradle/
+-- codec/
|   +-- zlink-ext-netty/
+-- native/
|   +-- linux-x64/
|   +-- linux-x86_64/
|   +-- src/
+-- src/
|   +-- main/
|   |   +-- java/
|   |   |   +-- module-info.java
|   |   |   +-- systems/zlink/contracts/
|   |   |   +-- systems/zlink/runtime/
|   |   +-- resources/native/
|   |       +-- darwin-aarch64/
|   |       +-- darwin-x86_64/
|   |       +-- linux-aarch64/
|   |       +-- linux-x64/
|   |       +-- linux-x86_64/
|   |       +-- windows-aarch64/
|   |       +-- windows-x86_64/
|   +-- test/
|       +-- java/systems/zlink/
|           +-- contract/
|           +-- integration/
+-- tests/
|   +-- run_tests.sh
|   +-- certs/
+-- samples/
|   +-- Zlink.Samples/
|       +-- src/main/java/systems/zlink/samples/
+-- perf/
    +-- common/
    +-- multi/Zlink.BindingBench.Multi/
    +-- single/Zlink.BindingBench/
    +-- baseline/
    +-- results/
    +-- tests/
```

`contracts` is the only public Java API tree. `runtime`, `native`, and
`src/main/resources/native` are implementation and packaging trees. Samples,
perf, and application-facing tests must import `systems.zlink.contracts.*`,
not `systems.zlink.runtime.*`.

`systems/zlink/internal/` may exist only as a non-exported bridge between
contract-owned public helpers and runtime implementations. It is not a public
contract package and must not be imported by samples, perf, or applications.

### Public Contract Layout

This tree is the Java projection of the contract categories defined by the
[.NET binding blueprint](../dotnet/README.en.md). The group directories below are
physical source-file groups used to keep the Java tree readable at a .NET-like
level. They do not create additional public Java package names. For example,
`contracts/sockets/SocketEnums/SendResult.java` still declares
`package systems.zlink.contracts.sockets;`. This preserves Java package
conventions and public import paths while still giving reviewers the requested
file classification.

```text
systems/zlink/contracts/
+-- core/
|   +-- AtomicCounter.java
|   +-- Context.java
|   +-- ContextOption.java
|   +-- ContextOptions.java
|   +-- RoutingId.java
|   +-- Stopwatch.java
|   +-- Zlink.java
|   +-- ZlinkThread.java
|   +-- ZlinkVersion.java
+-- errors/
|   +-- Errors/
|       +-- *Exception.java
|       +-- *Result.java
|       +-- ErrorCode.java
+-- eventing/
|   +-- EventEnums/
|   +-- EventHandlers/
|   +-- EventModels/
|   +-- MonitorSocket.java
|   +-- Poller.java
|   +-- Timer.java
+-- messaging/
|   +-- Message.java
|   +-- Received.java
|   +-- SubscriptionEntry.java
|   +-- SubscriptionEvent.java
|   +-- TopicMessage.java
+-- service/
|   +-- spot/
|       +-- Actor.java
|       +-- Spot.java
|       +-- SpotDispatchInfo.java
|       +-- SpotRoute.java
|       +-- SpotNode.java
|       +-- ActorJoinOperations/
|       +-- ActorManagementOperations/
|       +-- ActorModels/
|       +-- ServiceEnums/
|       +-- SpotNodeModels/
|       +-- SpotOperations/
|       +-- TopologyEnums/
+-- sockets/
    +-- Socket.java
    +-- StreamSocket.java
    +-- MessageSocketContracts/
    +-- PubSubSocketContracts/
    +-- RoutedSocketContracts/
    +-- SocketEnums/
    +-- SocketHandlers/
    +-- SocketOperations/
    +-- SocketOptionFacades/
```

Internal bridge files such as `systems/zlink/internal/ContractAccess.java` are
intentionally outside the `contracts/` tree because they are not exported and
are not application-facing API.

The group directories are not arbitrary feature buckets. They are Java source
file groups for the contract groups defined by the
[.NET binding blueprint](../dotnet/README.en.md). A new public contract file must
go into the smallest group that owns its concept.

Java public names should still be Java names. The C# `I` prefix is not copied:
`.NET` `ISocket.cs` maps to Java `Socket.java`, and `IStreamSocket.cs` maps to
Java `StreamSocket.java`.

### Runtime Layout

The runtime tree mirrors the public contract tree where that helps a reader
find the implementation owner. It is not public API, and JPMS must not export
it. Runtime files use normal Java package declarations that match their runtime
category.

```text
systems/zlink/runtime/
+-- core/
|   +-- NativeAtomicCounter.java
|   +-- NativeContext.java
|   +-- NativeCoreResources.java
|   +-- NativeCoreRuntime.java
|   +-- NativeRuntimeFactory.java
|   +-- NativeStopwatch.java
|   +-- NativeZlinkThread.java
+-- messaging/
|   +-- NativeMessageRuntime.java
|   +-- ReceivedPartCursor.java
+-- sockets/
|   +-- NativeSocketBase.java
|   +-- NativeSocketRuntime.java
|   +-- NativeSockets.java
|   +-- NativePairSocket.java
|   +-- NativeDealerSocket.java
|   +-- NativeRouterSocket.java
|   +-- NativePubSocket.java
|   +-- NativeSubSocket.java
|   +-- NativeXPubSocket.java
|   +-- NativeXSubSocket.java
|   +-- NativeStreamSocket.java
|   +-- NativeRouterReceiveSupport.java
|   +-- NativeRouterRequestSupport.java
|   +-- NativeRouterSpotSupport.java
|   +-- NativeStreamActorSupport.java
|   +-- SocketOperations.java
+-- eventing/
|   +-- NativeMonitorSocket.java
|   +-- NativePollEvents.java
|   +-- NativePoller.java
|   +-- NativeTimer.java
+-- service/
|   +-- spot/
|       +-- NativeActor.java
|       +-- NativeSpot.java
|       +-- NativeSpotNode.java
|       +-- SpotOptions.java
|       +-- SpotRoutedSupport.java
+-- errors/
|   +-- NativeErrorRuntime.java
+-- nativeapi/
    +-- Native.java
    +-- NativeLayouts.java
    +-- NativeMsg.java
    +-- NativeHelpers.java
    +-- NativeSymbols.java
    +-- LibraryLoader.java
    +-- InternalAccess.java
```

Runtime support files are allowed only when they hide real implementation
complexity inside one of the target runtime categories. They are not a
substitute for resource owners such as `NativeRouterSocket`, `NativeSpotNode`,
or `NativePoller`.

## Contract Interface Rule

Only native-backed resource behavior and staged operation behavior become
interfaces. Value-like types stay concrete.

### Must Be Public Interfaces

These are resource contracts. Runtime implementations must implement these
interfaces and must be created by factories.

- `Context`
- `Socket`
- `PairSocket`
- `DealerSocket`
- `RouterSocket`
- `PubSocket`
- `SubSocket`
- `XPubSocket`
- `XSubSocket`
- `StreamSocket`
- `MonitorSocket`
- `Poller`
- `Timer`
- `SpotNode`
- `Spot`
- Actor resource contracts when the Java surface exposes actor handles or actor
  lifecycle resources as native-backed handles

These are operation contracts because they hide staged multipart state, request state, or native submit
state:

- send operation
- publish operation
- request operation
- reply operation
- SPOT send/request/reply operation
- Actor create/join/reply/location operation
- stream actor bind/unbind/send operation

Handler roles may be interfaces or functional interfaces when
callers provide behavior to the runtime:

- SPOT dispatch handler
- actor lifecycle handler

### Must Stay Concrete

Do not create interfaces for these only for symmetry:

- `Message`
- `Received`
- `TopicMessage`
- `SubscriptionEvent`
- `RoutingId`
- options and filter value objects
- route result models
- snapshot models
- actor references
- enum/flag/result types
- exceptions

These are values or result objects. They may own native-backed storage
internally, but callers do not need substitutable behavior for them.

## Factory Entry Points

Construction is public only through contract factories. Direct construction of
runtime implementations is not part of the target API.

### Root Factory

`Zlink` belongs in `systems.zlink.contracts.core`.

Required root factory methods:

- `Zlink.createContext()`
- `Zlink.createPoller()`
- `Zlink.createTimer()`
- `Zlink.createTimer(Spot spot)`

`Zlink` may also own public static helpers such as version, capability,
strerror, proxy, shutdown, sleep, and auto-HWM recalculation. Those helpers may
delegate to runtime/native code, but their public signatures must not mention
runtime packages or native bridge types.

### Context Factories

`Context` is a public interface in `systems.zlink.contracts.core`.

Required context factory methods:

- `createPairSocket()`
- `createDealerSocket()`
- `createRouterSocket()`
- `createPubSocket()`
- `createSubSocket()`
- `createXPubSocket()`
- `createXSubSocket()`
- `createStreamSocket()`
- `createSpotNode(...)`

Every factory returns a public contract interface or concrete value type. It
never returns `NativeContext`, `NativeRouterSocket`, `NativeSpotNode`, or any
other runtime class.

### Service Factories

SPOT and Actor handles are created only by service methods on `SpotNode` or
other contract-owned service objects.

Allowed SPOT construction patterns:

- `SpotNode.createSpot(...)`
- `SpotNode.entrySpot()`
- `SpotNode.getOrCreateSpot(...)`
- `SpotNode.spotLookup(...)`

Allowed Actor construction patterns:

- `SpotNode.createActor(...)`
- actor factory/service methods explicitly owned by `SpotNode` or `Spot`

Direct public constructors for `Spot`, `SpotNode`, `Actor`, or runtime service
classes are not part of the target contract.

## Contract File Requirements

Contract files must be readable without knowing Panama, JNI, native handles,
native struct layouts, the completion registry, or raw
`*_part` loops.

Contract files may import:

- other `systems.zlink.contracts.*` packages;
- JDK types needed for public signatures, such as `Duration`,
  `AutoCloseable`, `CompletionStage`, `Optional`, `List`, records, or
  functional interfaces;
- third-party public value types only when they are intentionally part of the
  public contract.

Contract files must not import:

- `systems.zlink.runtime.*`;
- `systems.zlink.runtime.nativeapi.*`;
- `java.lang.foreign.*` for public native/Panama details;
- runtime implementation classes;
- native handle wrappers;
- marshalling helpers;
- native completion drain helpers.

The only exception is a public factory facade such as `Zlink` if Java chooses
direct static construction wiring. Even then, runtime references must be
private implementation details in method bodies and must not appear in public
signatures. Prefer a small runtime factory bridge when that keeps the contract
file clean.

## Runtime Implementation Requirements

Runtime classes implement public contract interfaces. They must not introduce
extra user-observable behavior that cannot be found from the contract
interface, concrete value type, or documented factory.

Runtime classes may be `public` for Java package or JPMS mechanics, but they
are not public API because `systems.zlink.runtime.*` is not exported. Samples,
perf, applications, and contract tests must not import them.

Runtime owns:

- native handle lifecycle;
- close and idempotent cleanup rules;
- native downcalls;
- native struct mirrors;
- message marshalling;
- socket-local provisional completion registry and drain owner;
- receive cursors;
- part-loop sequencing;
- native error mapping;
- typed option mapping;
- native resource adoption and release;
- service snapshots decoded from native data.

If a runtime implementation needs package-private access to a concrete value
object, use a narrow internal bridge owned by runtime/nativeapi. Do not expose
native handles or internal fields in the public contract.

## Socket Contract Shape

All socket resource contracts extend `Socket`. The common `Socket` owns only capabilities shared by
every socket:

- typed common options through `options()`;
- monitor open through `monitorOpen()`, `monitorOpen(MonitorEventType...)`, and
  `monitorOpen(long, MonitorEventType...)`;
- TLS server configuration through `setTlsServer(...)` and TLS client configuration through
  `setTlsClient(...)`;
- `close()`.

The family-specific or socket-specific contract that supports them owns `bind`, `connect`, `unbind`,
`disconnect`, `disconnectRid`, and ChannelName operations. The common `Socket` does not own them.

A typed socket contract adds only behavior meaningful for that socket type:

- `PairSocket`: send and recv.
- `DealerSocket`: send, recv, request.
- `RouterSocket`: routed send, routed recv, request, reply, and SPOT routing.
- `PubSocket`: publish.
- `SubSocket`: subscribe and subscription-event receive.
- `XPubSocket`: publish and subscription-event receive.
- `XSubSocket`: send and subscription control defined by the public binding contract.
- `StreamSocket`: RAW recv, PACKET recv, stream send, actor gateway, and bound-actor operations.

Protocol envelope helpers, raw native part submission, and native routing-ID pointers are not public
contract members.

## Operation Builder Shape

An operation builder is a public interface because it hides mutable staged state. It lives in the
category that owns the operation.

A builder start method accepts only a target identifier and reply token:

- `send()`
- `send(routingId)`
- `publish(topic)`
- `request()`
- `request(routingId)`
- `reply(routingId, replyToken)`
- `sendToSpot(nodeRid, spotRid)`
- `requestToSpot(nodeRid, spotRid)`
- `replyToSpot(nodeRid, spotRid, replyToken)`
- `sendBoundActor(sessionRid, actorId)`

PAIR, DEALER, ROUTER, and STREAM send builders use the `SendOperation` family. Send provides
asynchronous `submit()` and synchronous `submit_sync()`. Request provides `submit()` and
`submit_sync()` and sets its reply timeout on the builder. PUB/XPUB publish uses the same staged
message-builder pattern, but its `submit()` is synchronous `void` and immediately throws
`ZlinkSubmitException` on failure.

`submit_sync()` uses Core `NONE`; `submit()` uses Core `DONTWAIT` completion. The Kotlin
framework connects `CompletionStage` to its `await()` boundary. Direct Java binding use can also
use the synchronous terminal. The
[Pull completion public contract](#pull-completion-public-contract) contains the exact signatures.

PUB/XPUB publish `submit()` creates no `CompletionStage`. Default lossy publish succeeds by
dropping the copy for a subscriber whose queue is full; `NODROP` returns an immediate error.

The terminal for a raw ROUTER/`Received` reply is the synchronous one-shot
`ReplySubmitOperation.submit() -> void`. It creates no `CompletionStage`, accepts no send flags,
and submits a terminal reply or error reply with one native call. A DEALER peer is subject to
Application HWM, `PAUSED`, and `SNDTIMEO`, so the result can be `BACKPRESSURED`; a ROUTER peer uses
the HWM-free Completion connection. `NOT_CONNECTED`, `TERMINATED`, `INVALID_ARGUMENT`, and other
submit failures are delivered immediately as `ZlinkSubmitException`.

### Completion pull

Completion-backed state is registered in a provisional registry before native `FINAL`. A stage or
blocking request completes exactly once after native submit-outcome publication joins completion
capture. Stage cancellation completes only the waiter; the socket-local drain owner releases payload
and state on late completion.

Do not add separate operation-start families such as `sendNoWait`, `sendWithFlags`,
`requestAsync`, `publishWithFlags`, or a `send(message)` shortcut.

## Messaging Values

`Message`, `Received`, `TopicMessage`, and `SubscriptionEvent` are concrete
contract types.

`Message`:

- owns or shares message payload according to documented ownership rules;
- exposes Java-friendly factories such as `Message.from(...)`;
- must not expose raw `wrapNative`, `wrapDirect`, native pointer, or borrowed
  Java-buffer send paths as public API;
- a receive wrapper may return to a bounded `ThreadLocal` pool only after its
  owner `Received` removes part references and completes `close()`;
- the `Message` reference is invalid when deterministic cleanup returns. The
  caller must not use it again, including repeated `close()`, payload access,
  or identity-based `Map`/`WeakMap` lookup. A wrapper that has not been returned
  and a caller-created owned `Message` are not reused for another ownership.

`Received`:

- is reusable caller-provided receive storage;
- owns received message parts until closed or adopted;
- may carry routing ID, SPOT routing ID, `ReplyToken`, and reply sender
  metadata;
- does not expose native receive cursors or native handles.

`TopicMessage` and `SubscriptionEvent`:

- are concrete result/storage types;
- are filled through public receive/subscribe APIs;
- do not expose raw native topic buffers.

## Receive And Subscribe Shape

Data-plane receive APIs use caller-provided storage and return `boolean`.

Examples of target shape:

```java
Received received = new Received();
boolean ok = router.recv(received, RecvFlags.DONT_WAIT);
```

No-data is a normal `false` result for caller-provided no-wait receive.
Hard receive failures throw the documented exception type.

Core byte-HWM charge ends when ordinary `recv(...)` or `subscribe(...)`
dequeues the payload. `Received` and `TopicMessage` own only the Java lifetime
of parts, routing ID, `ReplyToken`, topic, and multipart framing. Closing
or reusing the output cleans up payload and metadata but does not participate
in Core HWM accounting. No separate retained receive, raw lease handle,
application byte capacity, or duplicate accounting state exists in a public or
internal API.

SPOT readable dispatch events are readiness notifications. Callers drain the
corresponding receive API until no-data.

Service control/admission receive APIs may use `Optional`, nullable, or typed
result-return forms when those are clearer than reusable data-plane storage.
They still must distinguish no-data from hard receive failure.

`ReceiveRecord.sourceBindingGeneration()` returns the validated binding
generation for a record sent from a bound STREAM session to an Actor. For that
record, `sourceSpotRid()` returns the session routing ID. Other records preserve
the zero value supplied by Core.

The binding decodes a Mesh dispatch `SEND_READY` record as
`MeshSendReadyData`. This value preserves Core's destination kind, target node
RID, target Spot RID, target Actor ref, and channel name. Fields that do not
apply to the destination kind retain the empty value supplied by Core.
`ReceiveRecord.sendReady()` returns this value only when the kind data has that
type and returns `null` for other record kinds.
This record kind belongs to the service-wire dispatch protocol; it is not a
Core HWM send-ready callback or an asynchronous-send completion.

## Handler Registration Naming

Handler registration names describe registration, not event occurrence.

- Use `set...Handler` for a single active handler per subject.
- Calling the same setter again replaces the handler.
- Use `add...Handler` or `register...Handler` only when the public contract
  intentionally supports multiple active handlers.
- Do not use `on...` as the canonical public registration name.

Canonical Java names:

- `setDispatchHandler`
- `recvRouted`
- `recvActorLifecycle`

## Byte HWM And Monitoring ABI v4

- An HWM is not the number of queued messages but the limit on accounted bytes Core computes.
- The Java public interface accepts byte values from `0` through
  `Long.MAX_VALUE`. It rejects negative input before calling Core and reports
  a Core value above `Long.MAX_VALUE` as an overflow error.
- `0` means unlimited, and the manual default is `4_096_000 bytes`.
- The former `int` overload, an alias, or a count-unit adapter is not provided.

```java
public final class ContextOptions {
    public long coreHwmMemoryLimitBytes();
    public void coreHwmMemoryLimitBytes(long value);
    public long coreHwmBudgetBytes();
    public void coreHwmBudgetBytes(long value);
    public CoreHwmProfile coreHwmProfile();
    public void coreHwmProfile(CoreHwmProfile value);
}

public interface Context {
    CoreHwmBudgetSnapshot coreHwmBudgetSnapshot();
    void resetCoreHwmBudgetMetrics();
}

public class CommonSocketOptions {
    public long sendHwm();           // Returns the non-negative outbound accounted-byte limit.
    public void sendHwm(long value); // Passes 0 through Long.MAX_VALUE to Core.
    public long recvHwm();
    public void recvHwm(long value);
}
```

Input precedence is manual Core budget, explicit memory limit, maximum JVM heap
hint, then Core fallback. Setting either of the first two values disables
automatic JVM-hint detection. The binding does not combine the hint with Core's
hard limit. If an explicit input exceeds a finite hard limit Core detected, the
binding preserves the existing configuration exception corresponding to
`EINVAL` and does not clamp the value.

Core applies the profile ratio to the memory limit exactly once, or uses an
explicit Core budget unchanged, then calculates planned byte HWM per physical
directional queue. A direction on which the caller sets `sendHwm(...)` or `recvHwm(...)`
becomes a manual override and is not changed by later automatic HWM
recalculation.

The Java binding does not recount queued messages or payloads. When the actual
accounted bytes in a Core pipe reach the applied HWM, the native submit result
reports backpressure and the Java operation preserves it through the existing
result and timeout contract. A `long` value of `0` means unlimited. Negative
values are not valid HWM inputs.

`monitorOpen(monitorHwmBytes, events...)` accepts a non-negative `long` byte
value for the monitor queue. Zero selects the Core monitor default; a positive
value is forwarded unchanged. Java and Kotlin expose no message-count overload
or alias.

- The `MonitorStatus` record exposes the same fields as native `zlink_monitor_status_t` ABI version 4.
- Planned, applied, and deferred HWMs, and in-flight usage, are non-negative
  `long` byte values; a larger Core value is an overflow error.
- A deferred value is meaningful only when its matching `autoHwmDeferredSendHwmValid()` or `autoHwmDeferredRecvHwmValid()` method returns `true`.
- A pending-message value remains a count diagnostic and does not share a name with a byte field.
- Pending bytes are exposed separately through `sndPendingBytes()` and `rcvPendingBytes()`.
- A snapshot whose `abiVersion()` is not `3`, or whose `structSize()` differs from the binding layout, raises `UnsupportedOperationException`. An older monitoring layout is not accepted.

`CoreHwmBudgetSnapshot` projects ABI version/size, configured/runtime/resolved
memory limits, configured/effective budgets, planned/applied/manual-reserved
HWM, Core-queue/application/current/peak/provisional accounted bytes,
completion current/peak/pending and total-messaging values, monitor/instance
aggregates, application/completion queue counts,
`outstandingApplicationLeaseCount()`, `retiredQueueCount()`,
`deferredOriginCreditBytes()`, oversize/blocked/aggregate flags,
`budgetGeneration()`, and `measurementEpoch()` without unit conversion. Reset
preserves current, pending, and queue-count gauges, rebases both peaks to
current, clears epoch counters, and increments `measurementEpoch`.
`applicationAccountedBytes()` and the three owner-lifecycle fields are
ABI-reserved and always zero. A budget snapshot ABI version/size mismatch
raises `UnsupportedOperationException`.

Java and Kotlin call the same Java methods. No Kotlin-only adapter or option
with a different unit is added. Request/reply APIs do not take an HWM argument
and retain their existing lifetime and ownership contract.

## Receive flow state

The binding exposes the Core receive-flow state as the `ReceiveFlowState`
enum with `RUNNING(0)` and `PAUSED(1)`. The public setter is the common
socket-option facade method `receiveFlowState(ReceiveFlowState)`. It returns
`void` and follows the Java error policy: a non-zero native result is thrown as
a `ZlinkConfigException` carrying the matching `ConfigResult`, so a socket
that doesn't support receive flow raises `ZlinkConfigException` with the
not-supported result. A null argument raises `NullPointerException` before any native call.
Setting the state the socket already holds returns normally.

The observation surface follows the C contract, so the constant and metric
names are fixed by the C layer: the monitor events `SEND_FLOW_PAUSED`,
`SEND_FLOW_RESUMED`, and `FLOW_STATE_STALE` (`1 << 16`, `1 << 17`, `1 << 18`,
with the full mask `0x7FFFF`), the event flags `SEND_FLOW_WRITABLE` (`1 << 1`),
and `FLOW_STATE_STALE_EPOCH` (`1 << 3`), the status detail bit `FLOW_STATE`
(`1 << 5`), and the five status
fields `flow_paused_connections`, `flow_pause_applied_total`,
`flow_resume_applied_total`, `flow_state_stale_total`, and
`flow_pause_duration_ms`, projected with this language's naming convention.

Flow-state frames stay inside Core. The binding calls the setter, reads the
monitor events and the snapshot fields, and never encodes, decodes, sends, or
receives a flow-state frame itself.

## Error And Result Policy

Java public errors preserve core result-domain meaning but do not expose native
errno as the primary user API.

- Fixed-size boundary values are validated before native calls.
- Routing ids, actor ids, endpoints, channel names, and topics are not silently
  truncated.
- `SubmitException`, `RecvException`, `RequestException`,
  `ConfigException`, and other typed exceptions preserve the relevant public
  result values.
- Native errno and platform-specific error text may appear as diagnostic
  detail, not as the main public contract.

## Spot And Actor Contract Shape

SPOT service contracts live under `systems.zlink.contracts.service.spot`.

`SpotNode` is the owner for:

- node lifecycle;
- service registration;
- peer/channel configuration;
- route lookup;
- spot creation and lookup;
- actor creation;
- actor route lookup;
- actor lifecycle receive;
- SPOT dispatch receive.

`Spot` is the handle contract for SPOT-level send/request/reply, publish,
dispatch, actor operation entrypoints, and timer integration.

Actor and SPOT route results are concrete contract models:

- `ActorRoute` preserves resolved Actor ref, Actor node RID, current Spot RID,
  and current Spot kind.
- `SpotRoute` preserves Spot RID, owner node RID, and Spot kind.
- `SpotKind` distinguishes Entry Spot from user Spot.
- Invalid kind is not a successful route result.

- Java exposes `SpotNode.sendToActor(ActorRef)` and `SpotNode.requestToActor(ActorRef)`, taking a resolved Actor ref as their argument.
- The send operation hands off ownership of one or more message parts once submit succeeds, and completes once the Actor owner mailbox accepts the handoff.
- The request operation hands off ownership of the request part once submit succeeds, and delivers the reply part the Actor handler produced.
- Java does not revive a removed Discovery route table or resolver API as a compatibility helper.

## Spot Get-Or-Create

Java exposes `SpotNode.getOrCreateSpot(RoutingId)`. It maps directly to
`zlink_spot_node_spot_get_or_new(...)`; it must not be implemented by
composing `spotLookup` and `createSpot`.

The method returns a concrete result containing the caller-owned `Spot`
contract and a `created` boolean. `created` is `true` only for the call that
created the logical spot.

## Performance Policy

Hot paths must not use reflection, dynamic method lookup, classpath scanning,
avoidable allocation, avoidable buffer copies, hidden waits, sleeps, busy
waits, broad locks, or thread joins.

Native method-handle setup happens during initialization, not in the per-message processing loop.

Native bridge code should materialize Java values directly from core receive
substrates. Public contract code should not contain raw native receive loops.

Perf, samples, and tests use exported public contract packages only.

## Architecture requirements

The Java binding maintains these boundaries:

1. Define the public resource interfaces under `systems.zlink.contracts.*`.
2. Keep value/model/result/exception types concrete in their contract
   category.
3. Move native-backed concrete resource classes to `systems.zlink.runtime.*`
   and rename them with implementation-oriented names such as `NativeContext`
   or `NativeRouterSocket`.
4. Make runtime classes implement the contract interfaces.
5. Move factory entrypoints to public contract types and make them return
   contract interfaces.
6. Native-backed resources have no direct public constructors.
7. Runtime/nativeapi or runtime support classes own native handles, Panama/JNI calls, completion drain,
   marshalling helpers, and part loops.
8. Samples, perf, tests, and documentation examples import only
   `systems.zlink.contracts.*`.
9. Compatibility aliases and deprecated wrappers for a direct-concrete shape are not part of the
   public contract.
10. Verify JPMS exports expose only contract packages.

Do not start by only extracting helper classes from concrete contract
resources. That hides implementation details but leaves the wrong public
resource design in place.

## Implementation Checklist

The Java binding is aligned only when all items are true:

- `Context`, sockets, eventing resources, SpotNode, Spot,
  and Actor native-backed resources are public contract interfaces.
- Runtime native-backed implementations live under `systems.zlink.runtime.*`.
- Factory entrypoints return contract interfaces and hide runtime class names.
- Runtime packages are not JPMS-exported.
- Contract files, except narrowly justified factory wiring, do not import
  `systems.zlink.runtime.*`.
- Public signatures do not mention native handles, Panama memory segments,
  native bridge types, completion-registry state, or raw part loops.
- DTO/value/record/enum/result/exception types remain concrete.
- Operation builders are public contracts and hide staged state.
- Samples, perf, tests, and applications import only
  `systems.zlink.contracts.*`.
- Native-backed resources have no direct public construction paths.
- The public surface has no compatibility wrappers, earlier-name aliases, or deprecated duplicate
  operation names.
- Public contract package and file layout matches the category map in this
  document.

## Verification

Run verification from `bindings/java/`.

Required baseline:

- `./gradlew build`
- `./tests/run_tests.sh`

Run sample verification when construction paths, public examples, or resource
lifecycles change:

- `./samples/run_samples.sh`

Run perf smoke gates when send, receive, request, poller, timer, service, or
hot-path behavior changes:

- `./perf/single/run_benchmarks.sh`
- `./perf/multi/run_benchmarks.sh`

Required structural searches:

```sh
rg -n "exports systems\\.zlink\\.runtime" src/main/java/module-info.java
rg -n "import systems\\.zlink\\.runtime\\." src/test samples perf -g'*.java'
rg -n "import systems\\.zlink\\.runtime\\." src/main/java/systems/zlink/contracts -g'*.java'
rg -n "java\\.lang\\.foreign|MemorySegment|Native[A-Za-z]*|RequestProgressPump" \
  src/main/java/systems/zlink/contracts -g'*.java'
```

The first three searches must return no public-surface leaks. The last search
may only return intentionally concrete value internals after review; it must
not show public resource interfaces or operation contracts depending on native
bridge details.

## Pull completion public contract

The Java package uses Core 0.16.0 as an exact dependency.

The Java runtime drains native completions and converts them into blocking results or
`CompletionStage`. `submit_sync()` uses Core `NONE`; `submit()` uses Core `DONTWAIT`. Kotlin uses this
Java contract without creating an independent native ABI or token wrapper.

Completion-backed state is registered in a provisional registry before native `FINAL`. A
`CompletionStage` or blocking request completes exactly once after submit-outcome publication and
completion capture have both finished. Stage cancellation ends only the caller wait and does not cancel
the Core operation; a late completion releases the native payload.

`PollEventFlags.POLLCOMPLETION` is a progress event indicating that the public poller's wait thread
drained the native queue and fully processed at least one live stage or detached state. Under public
poller ownership, using a blocking request requires another thread to continue executing the wait loop.

Only ROUTER REQUEST receive creates a `ReplyToken`. During class initialization, it registers a private
constructor method reference with non-exported `ContractAccess.ReplyTokenAccess`. Equality and hashing
use both owner identity and an opaque value. `StreamPacket` is an empty reusable output. A token provides
no raw accessor, ordering, serialization, or `AutoCloseable`. Concurrent recv into the same output is
invalid-state. Message references remain valid only until the next recv entry or `close()`. Before the
first bind/connect, the `recvMode` setter accepts only `RAW` and `PACKET` and rejects `UNSPECIFIED`.

### Public interface

```java
public interface SendSubmitOperation {
    SendSubmitOperation message(Message part);
    CompletionStage<Void> submit();
    void submit_sync();
}

public interface RequestSubmitOperation {
    RequestSubmitOperation message(Message part);
    RequestSubmitOperation timeout(Duration timeout);
    CompletionStage<List<Message>> submit();
    List<Message> submit_sync();
}

public final class ReplyToken {
    private final Object owner;
    private final long value;

    private ReplyToken(Object owner, long value) {
        this.owner = owner;
        this.value = value;
    }

    @Override public boolean equals(Object other) {
        return other instanceof ReplyToken token
            && owner == token.owner && value == token.value;
    }
    @Override public int hashCode() {
        return 31 * System.identityHashCode(owner) + Long.hashCode(value);
    }
    @Override public String toString() { return "ReplyToken"; }
}

public interface StreamSocket {
    SendOperation send(RoutingId rid);
    boolean recv(Received out, RecvFlags flags);
    boolean recvPacket(StreamPacket out, RecvFlags flags);
}

public enum StreamRecvMode {
    UNSPECIFIED,
    RAW,
    PACKET
}

public final class StreamSocketOptions {
    public StreamRecvMode recvMode();
    public void recvMode(StreamRecvMode mode);
}

public interface ReplySubmitOperation {
    ReplySubmitOperation message(Message part);
    void submit();
}

public final class StreamPacket implements AutoCloseable {
    public StreamPacket();
    public boolean isEmpty();
    public Optional<RoutingId> routingId();
    public Message header();
    public Message body();
    @Override public void close();
}
```

The operation-start signatures are PAIR `SendOperation send()`, DEALER
`SendOperation send()` and `RequestOperation request()`, ROUTER
`SendOperation send(RoutingId)`, `RequestOperation request(RoutingId)`, and
`ReplyOperation reply(RoutingId, ReplyToken)`, and STREAM `SendOperation send(RoutingId)`. A send
factory captures the target in the builder. `Received.replyToken()` returns `Optional<ReplyToken>`.
`Received.send()` returns a `SendOperation` that captures the source target, and `Received.reply()`
returns a `ReplyOperation` that captures the source RID and token.

The public Java/Kotlin surface contains no `AsyncSend*` or `RoutedSend*` operation type,
`StreamSocket.sendAsync()`, send/request flags, request `BiConsumer` terminal, STREAM `onPacket`,
monitor `onEvent` or ignore, timer `onFire`, or pair/generation member.

Monitor provides `MonitorEvent recv()`, nullable `recv(RecvFlags)`, `MonitorStatus status()`, and
`close()`. Timer provides `start(Duration, long)`, `stop()`, `long recv()`, and `close()`. Monitor
DONTWAIT no-data is `null`; timer no-data is a typed receive exception. Monitor-event `connectionId` is
used only for diagnostics and correlation, not as a send/reply target or reconnect fence. The internal
native enum mirror uses only `ZLINK_OPT_PENDING_MAX_MSGS` and `ZLINK_OPT_PENDING_MAX_BYTES` and adds no
public option method.

## Implementation and contract-test verification requirements

Verify the following using only the public Java interface, `CompletionStage`, exceptions, and poller
events. Each item maps to one contract test.

**Operations and completion**

- PAIR, DEALER, ROUTER, and STREAM send factories return one `SendOperation` family.
- Send/request expose only the flag-free async and synchronous terminals in the Public interface section
  and retain request timeout.
- Even when completion drains before submit returns, the stage completes exactly once after joining
  submit publication.
- After stage cancellation, a late completion does not complete the stage again and releases the native
  payload.
- A non-OK request completion exposes only a typed request exception and does not expose the error
  payload.
- `POLLCOMPLETION` returns only after stage settlement or detached cleanup finishes.
- When HWM/`PAUSED` waiting expires for a raw reply submitted to a DEALER peer,
  `ZlinkSubmitException` reports `BACKPRESSURED`; a reply submitted to a ROUTER
  peer retains the HWM-free result of the Completion connection.

**ReplyToken and STREAM**

- Only ROUTER REQUEST receive returns a non-empty token, and only tokens with the same owner and value
  have matching equality and hash.
- Reply with a token owned by another socket fails before the native call.
- `StreamPacket` holds a payload after success, is empty on no-data or error, and can be reused after
  `close()`.

**Pull eventing and Kotlin**

- Monitor and timer recv return their specified no-data results, events, and fire counts without
  callbacks.
- Kotlin source uses the Java `ReplyToken`, operation terminals, and STREAM packet interface directly,
  without another wrapper.
