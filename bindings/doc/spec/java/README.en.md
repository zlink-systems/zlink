---
title: "Java Binding Implementation Blueprint"
---

<!-- bindings-nav:start -->
[Spec index](../README.md) | [Previous: C++](../cpp/README.md) | [Next: Node.js](../node/README.md)
<!-- bindings-nav:end -->

# Java Binding Implementation Blueprint

> **What this chapter defines** — the target `contracts`/`runtime` shape the
> Java binding must have, and the JPMS export boundary.

This document defines the target Java binding shape. It is not an exhaustive
method reference. The exact public member list belongs in
`bindings/java/src/main/java/systems/zlink/contracts/` after the refactor is
complete.

The Java binding is aligned only when it uses the same architecture map as the
.NET binding:

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

This is a breaking target. Do not keep compatibility shims, deprecated
wrappers, duplicate construction paths, public runtime aliases, or direct
constructors only to preserve the old Java surface.

| Section | Covers |
|---|---|
| [Source Of Truth](#source-of-truth) | The semantic source of truth and the Java repository ownership boundary |
| [Current Refactor Rule](#current-refactor-rule) | The test for "the right direction" while the refactor is in progress |
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
| [Receive And Subscribe Shape](#receive-and-subscribe-shape) | Caller-provided storage and the no-data distinction |
| [Handler Registration Naming](#handler-registration-naming) | The `set...Handler` naming rule |
| [Byte HWM And Monitoring ABI v2](#byte-hwm-and-monitoring-abi-v2) | Unsigned `long` HWM and monitor snapshot fields |
| [Error And Result Policy](#error-and-result-policy) | Typed exceptions and validation timing |
| [Spot And Actor Contract Shape](#spot-and-actor-contract-shape) | `SpotNode`/`Spot` responsibilities and route results |
| [Spot Get-Or-Create](#spot-get-or-create) | The `getOrCreateSpot` contract |
| [Performance Policy](#performance-policy) | Hot-path constraints |
| [Refactor Workflow](#refactor-workflow) | The order of alignment work |
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

## Current Refactor Rule

During the Java refactor, code may temporarily be in transition, but the target
shape is fixed. A change moves in the right direction only if it makes one of
these statements more true:

- a native-backed resource is typed by a public contract interface;
- a native-backed implementation moves under `systems.zlink.runtime.*`;
- a factory returns a contract type and hides the runtime class;
- a public contract file no longer imports `systems.zlink.runtime.*`;
- a sample, perf runner, or test no longer imports runtime packages;
- a native handle, raw part loop, callback trampoline, request pump, or native
  struct mirror moves out of public contract source.

Moving only a native helper while leaving the main public resource as a
concrete contract class is not sufficient. The .NET-standard target is a
contract/runtime split at the resource boundary, not only at the helper
boundary.

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
downcalls, marshalling, callback bridge state, request progress, socket
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
| `systems.zlink.runtime.messaging` | Message materialization, multipart progress, request progress, and request execution. |
| `systems.zlink.runtime.sockets` | Socket kernels, socket family implementations, callback adapters, and socket operation execution. |
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

These are operation contracts because they hide staged multipart state, request
state, callback state, or native submit state:

- send operation
- routed send operation
- publish operation
- request operation
- reply operation
- SPOT send/request/reply operation
- Actor create/join/reply/location operation
- stream actor bind/unbind/send operation

Handler and callback roles may be interfaces or functional interfaces when
callers provide behavior to the runtime:

- socket receive handler
- send-ready handler
- stream packet handler
- monitor handler
- timer handler
- SPOT dispatch handler
- actor lifecycle handler
- request callback
- reply callback

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
native struct layouts, callback userdata, request pump threads, or raw
`*_part` loops.

Contract files may import:

- other `systems.zlink.contracts.*` packages;
- JDK types needed for public signatures, such as `Duration`,
  `AutoCloseable`, `CompletableFuture`, `Optional`, `List`, records, or
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
- request progress helpers.

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
- request progress pumps;
- callback trampolines;
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

Socket contracts are interfaces. They should expose behavior, not native
transport mechanics.

Common socket behavior belongs in `Socket`:

- `bind`
- `connect`
- `unbind`
- `disconnect`
- `disconnectRid`
- `setChannelName`
- `getChannelName`
- `options`
- `setSendReadyHandler`
- `close`

Typed socket contracts add only capabilities that are meaningful for that
socket type:

- `PairSocket`: send and recv.
- `DealerSocket`: send, recv, request.
- `RouterSocket`: routed send, routed recv, request, reply, SPOT routing.
- `PubSocket`: publish.
- `SubSocket`: subscribe and subscription event receive.
- `XPubSocket`: publish plus subscription event receive.
- `XSubSocket`: send and subscription control as defined by the public
  binding contract.
- `StreamSocket`: stream send/recv, packet handler, actor gateway, bound actor
  operations.

Do not expose protocol envelope helpers, request tokens, raw native part
submission, callback userdata, or native routing-id pointers.

## Operation Builder Shape

Operation builders are public interfaces because they hide mutable staged
state. They live in the category that owns the operation.

Builder start methods take only the target identity:

- `send()`
- `send(routingId)`
- `publish(topic)`
- `request()`
- `request(routingId)`
- `reply(routingId, requestSequence)`
- `sendToSpot(nodeRid, spotRid)`
- `requestToSpot(nodeRid, spotRid)`
- `replyToSpot(nodeRid, spotRid, requestSequence)`
- `sendBoundActor(sessionRid, actorId)`

Payload, flags, timeout, callback, and async behavior are builder steps.
Representative terminal methods:

- `submit()`
- `await()`
- `submit(callback)`

`submit()` starts the async operation and returns `CompletionStage`; `await()`
is the adapter that waits for the same operation on the current thread. The
shared language policy is defined in
[bindings async execution surface policy](../async-coroutine-policy.md).

Do not add separate operation-start families such as `sendNoWait`,
`sendWithFlags`, `requestAsync`, `publishWithFlags`, or direct
`send(message)` shortcuts. Use one operation name and let the builder absorb
the variation.

## Messaging Values

`Message`, `Received`, `TopicMessage`, and `SubscriptionEvent` are concrete
contract types.

`Message`:

- owns or shares message payload according to documented ownership rules;
- exposes Java-friendly factories such as `Message.from(...)`;
- must not expose raw `wrapNative`, `wrapDirect`, native pointer, or borrowed
  Java-buffer send paths as public API.

`Received`:

- is reusable caller-provided receive storage;
- owns received message parts until closed or adopted;
- may carry routing id, SPOT routing id, request sequence, and reply sender
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

## Handler Registration Naming

Handler registration names describe registration, not event occurrence.

- Use `set...Handler` for a single active handler per subject.
- Calling the same setter again replaces the handler.
- Use `add...Handler` or `register...Handler` only when the public contract
  intentionally supports multiple active handlers.
- Do not use `on...` as the canonical public registration name.

Canonical Java names:

- `setSendReadyHandler`
- `setPacketHandler`
- `setDispatchHandler`
- `recvRouted`
- `recvActorLifecycle`

## Byte HWM And Monitoring ABI v2

- An HWM is not the number of queued messages but the limit on accounted bytes Core computes.
- The Java public interface interprets all 64 bits of a `long` as an unsigned value, carrying the full range of Core's `uint64_t` without loss.
- A Java caller uses `Long.compareUnsigned` and `Long.toUnsignedString`; a Kotlin caller converts with `ULong.toLong()` and `Long.toULong()`.
- `0` means unlimited, and the manual default is `4_096_000 bytes`.
- The former `int` overload, an alias, or a count-unit adapter is not provided.

```java
public final class ContextOptions {
    public long autoHwmMessageUnitBytes();           // Returns an unsigned 64-bit planning unit.
    public void autoHwmMessageUnitBytes(long value); // Zero selects the socket-type default.
}

public class CommonSocketOptions {
    public long sendHwm();           // Returns the unsigned outbound accounted-byte limit.
    public void sendHwm(long value); // Passes all 64 bits of value to Core.
    public long recvHwm();
    public void recvHwm(long value);
}
```

- The `MonitorStatus` record exposes the same fields as native `zlink_monitor_status_t` ABI version 2.
- Planned, applied, and deferred HWMs, and in-flight usage, are unsigned `long` byte values.
- A deferred value is meaningful only when its matching `autoHwmDeferredSendHwmValid()` or `autoHwmDeferredRecvHwmValid()` method returns `true`.
- A pending-message value remains a count diagnostic and does not share a name with a byte field.
- A snapshot whose `abiVersion()` is not `2`, or whose `structSize()` differs from the binding layout, raises `UnsupportedOperationException`. The former 32-bit monitoring layout is not accepted.

Java and Kotlin call the same Java methods. No Kotlin-only adapter or option
with a different unit is added. Request/reply APIs do not take an HWM argument
and retain their existing lifetime and ownership contract.

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

Callback stub and method-handle setup may happen during registration, not in
the per-message processing loop.

Native bridge code should materialize Java values directly from core receive
substrates. Public contract code should not contain raw native receive loops.

Perf, samples, and tests use exported public contract packages only.

## Refactor Workflow

Use this order when aligning the Java binding:

1. Define the public resource interfaces under `systems.zlink.contracts.*`.
2. Keep value/model/result/exception types concrete in their contract
   category.
3. Move native-backed concrete resource classes to `systems.zlink.runtime.*`
   and rename them with implementation-oriented names such as `NativeContext`
   or `NativeRouterSocket`.
4. Make runtime classes implement the contract interfaces.
5. Move factory entrypoints to public contract types and make them return
   contract interfaces.
6. Remove direct public constructors for native-backed resources.
7. Move native handles, Panama/JNI calls, callback trampolines, request pumps,
   marshalling helpers, and part loops into runtime/nativeapi or runtime
   support classes.
8. Update samples, perf, tests, and documentation examples to import only
   `systems.zlink.contracts.*`.
9. Remove compatibility aliases and deprecated wrappers that preserve the old
   direct-concrete shape.
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
  native bridge types, callback userdata, request pumps, or raw part loops.
- DTO/value/record/enum/result/exception types remain concrete.
- Operation builders are public contracts and hide staged state.
- Samples, perf, tests, and applications import only
  `systems.zlink.contracts.*`.
- No direct constructors for native-backed resources remain as public
  construction paths.
- No compatibility wrappers, old aliases, or deprecated duplicate operation
  names remain.
- Public contract package and file layout matches the category map in this
  document.

## Verification

Run verification from `bindings/java/` after the refactor.

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
