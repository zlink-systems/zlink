---
title: "13. Key Type Usage Index · Kotlin"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: Operations — metrics · drain · readiness](12-operations.en.md) | [Next: Picking A Sample](14-samples.en.md)
<!-- framework-adapter-nav:end -->

# 13. Key Type Usage Index

> **The document that owns this chapter's contract** — most of this surface is owned by
> [Java 13. Key Interface Usage Index](../../../java/guide/server/13-interface-catalog.en.md).
> This chapter gathers only what Kotlin **additionally provides.**

Read the Java chapter first. Any name not listed here is the same as Java.

## 1. The Suspend Handler Contract

A Java handler returns a `CompletionStage`; its Kotlin counterpart is `suspend`.
**You can implement either one, and mix both in the same project** — registration checks
which contract it is and calls it accordingly.

| What it receives | Java | Kotlin |
| --- | --- | --- |
| A channel request | `ZLinkRequestHandler` | `ZLinkSuspendingRequestHandler` |
| A channel one-way send | `ZLinkSendHandler` | `ZLinkSuspendingSendHandler` |
| A classic fanout event | `ZLinkFanoutHandler` | `ZLinkSuspendingPublishHandler` |
| A Node direct request / send | `ZLinkRouteRequestHandler` · `ZLinkRouteSendHandler` | `ZLinkSuspendingRouteRequestHandler` · `ZLinkSuspendingRouteSendHandler` |
| A packet addressed to a Spot | `ZLinkSpotPacketHandler` | `ZLinkSuspendingSpotPacketHandler` |
| A request addressed to a Spot | `ZLinkSpotRequestHandler` | `ZLinkSuspendingSpotRequestHandler` |
| A subscription event | `ZLinkSpotSubscriptionHandler` | `ZLinkSuspendingSpotSubscriptionHandler` |
| A timer tick | `ZLinkSpotTimerHandler` | `ZLinkSuspendingSpotTimerHandler` |
| A packet / request addressed to a member Actor | `ZLinkSpotActorSendHandler` · `...RequestHandler` | `ZLinkSuspendingSpotActorSendHandler` · `...RequestHandler` |
| An Entry Spot's Actor packet / request | `ZLinkEntrySpotActorSendHandler` · `...RequestHandler` | `ZLinkSuspendingEntrySpotActorSendHandler` · `...RequestHandler` |
| A session typed packet | `ZLinkTypedSessionPacketHandler` | `ZLinkSuspendingTypedSessionPacketHandler` |

## 2. The `.kotlin()` Wrapper

Calling `.kotlin()` on a Java client turns the same call into a suspend surface.

| Java | What `.kotlin()` gives |
| --- | --- |
| `ZLinkClient` | `ZLinkKotlinClient` |
| `ZLinkRouteClient` | `ZLinkKotlinRouteClient` |
| `ZLinkFanoutClient` | `ZLinkKotlinFanoutClient` |
| `ZLinkActorClient` | `ZLinkKotlinActorClient` |
| `ZLinkActorManager` | `ZLinkKotlinActorManager` |

The call types the wrapper returns are also Kotlin counterparts —
`ZLinkKotlinRequestCall`, `ZLinkKotlinMessageSendCall`, `ZLinkKotlinActorCreateCall`,
`ZLinkKotlinLifecycleCall`, `ZLinkKotlinBoundSession`.

**The wrapper is optional.** You can use the Java surface as-is and receive it with §3's
`await()`.

## 3. Extension Functions

Extension functions fill in the slots where there's no wrapper.

| Extension | What it changes |
| --- | --- |
| `CompletionStage<T>.await()` | Receives the result as suspend. **Turn-aware** (§4) |
| `Flow.Publisher<T>.asFlow()` | Converts a status stream to a coroutine `Flow` |
| `ZLinkLocationRuntimeQuery.topology(filter, pageSize)` | Turns a page loop into a `Flow` |
| `ZLinkSpotHandlerRegistry.addHandler<T>()` | Register with a reified type |
| `ZLinkFrameworkOptions.routeMesh(name) { ... }` | MeshNode registration as a block |
| `ZLinkMeshNodeBuilder.channelName(name) { ... }` | Channel registration as a block |
| `ZLinkMeshPeerConnections.connect(...)` | Multiple endpoints at once |
| `ZLinkFrameworkOptions.configureDispatch { ... }` | Diagnostics configuration as a block |
| `ZLinkDispatchOptions.onMessageFlow { ... }` | An observer as a lambda |
| `ZLinkMessage.decode<T>()` · `messageOf(...)` | Reified decode and construction |
| `ZLinkStreamConnector.kotlin()` · `.messages()` · `.errors()` | The connector as suspend / `Flow` |
| `ZLinkStreamConnectorOptions.withLz4StreamCompression()` and others | Compression settings |

## 4. Don't Swap `await()` For Just Anything

`zlink-framework-kotlin`'s `CompletionStage<T>.await()` **is aware of the framework turn.**
Calling it inside a Spot's or an Actor's turn doesn't break that turn's guarantee of
serial execution.

`kotlinx.coroutines.future.await` has the same name, so a single import can switch which
one you get. If the code runs inside a turn, check which one you imported.

```kotlin
import systems.zlink.framework.kotlin.await   // The turn-aware one
```

## 5. Related Documents

- The interface index by feature: [Java 13. Key Interface Usage Index](../../../java/guide/server/13-interface-catalog.en.md)
- Kotlin layer overview: [1. Overview](01-overview.en.md) §2
- The Kotlin-specific contract: [Kotlin public contract](../../../common/spec/server/languages/kotlin/README.ko.md)
- The shared contract: [Java exact interface table of contents](../../../common/spec/server/languages/java/interfaces/README.ko.md)
