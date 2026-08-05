---
title: "13. Key Type Usage Index · Node/TypeScript"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: Operations — metrics · drain · readiness](12-operations.en.md) | [Next: Picking A Sample](14-samples.en.md)
<!-- framework-adapter-nav:end -->

# 13. Key Type Usage Index

> **The document that owns this chapter's contract** —
> the [Node.js exact interface table of contents](../../../common/spec/server/languages/node/interfaces/README.ko.md)
> owns the exact signatures. This chapter is a guide to finding the public surface an
> application uses often, organized by feature.

The Node surface splits first by **which package you import it from.**

| Package | What's in it |
| --- | --- |
| `@zlink-systems/framework` | Contract types — interface / type / enum |
| `@zlink-systems/nestjs` | Registration, decorators, injection tokens |

Contract types can be imported with `import type`. The only things that need a runtime
value are the decorators, the tokens, and `zlinkFramework()`.

## 1. Injection Tokens

Clients and runtimes are **injected by token.** The type alone doesn't tell Nest what to
inject.

```typescript
constructor(
  @Inject(ZLINK_ROUTE_CLIENT) private readonly client: ZLinkRouteClient
) {}
```

| Token | What's injected |
| --- | --- |
| `ZLINK_ROUTE_CLIENT` | send / request by ChannelName or Node RID |
| `ZLINK_CHANNEL_CLIENT` | Calls going out from inside a Spot/Actor |
| `ZLINK_FANOUT_CLIENT` | Classic fanout publish |
| `ZLINK_SPOT_PUBLISHER_CLIENT` | Publishing a Logical Multicast from outside a Spot |
| `ZLINK_SPOT_MANAGER` | Spot creation, lookup, close |
| `ZLINK_ACTOR_MANAGER` · `ZLINK_ACTOR_CLIENT` | Actor creation/lookup, ActorId calls |
| `ZLINK_SPOT_OUTBOUND` | Outbound from a Spot's context |
| `ZLINK_FRAMEWORK_RUNTIME` | Host status, plus relocate / shutdown |
| `ZLINK_ROUTE_MESH_RUNTIME` · `ZLINK_CLIENT_SERVER_RUNTIME` · `ZLINK_FANOUT_RUNTIME` | Each surface's status |
| `ZLINK_ROUTE_MESH_RUNTIME_OPTIONS` · `ZLINK_CHANNEL_RUNTIME_OPTIONS` | Adjusting weight while running |
| `ZLINK_LOCATION_RUNTIME_QUERY` | Location status and topology |
| `ZLINK_MESSAGE_METADATA_POLICY` | Metadata policy |
| `ZLINK_HTTP_CLIENT_REGISTRY` | The HTTP client |

## 2. Handler Decorators

There's one decorator per thing received. The first argument is the handler group, and the
next is the packet name or topic.

| Decorator | Matching contract |
| --- | --- |
| `zlinkRequestHandler(group, packet)` | `ZLinkRequestHandler<TReq, TRes>` |
| `zlinkSendHandler(group, packet)` | `ZLinkSendHandler<TMsg>` |
| `zlinkPublishHandler(group, packet)` | `ZLinkFanoutHandler<TEvent>` |
| `zlinkSpotPacketHandler(...)` | `ZLinkSpotPacketHandler<TSpot, TMsg>` |
| `zlinkSpotSubscriptionHandler(...)` | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` |
| `zlinkSpotTimerHandler(...)` | `ZLinkSpotTimerHandler<TSpot>` |
| `zlinkSpotActorSendHandler(...)` · `zlinkSpotActorRequestHandler(...)` | A packet / request addressed to a member Actor |
| `zlinkEntrySpotPacketHandler(...)` · `zlinkEntrySpotSubscriptionHandler(...)` | Addressed to the Entry Spot |
| `zlinkEntrySpotActorSendHandler(...)` · `zlinkEntrySpotActorRequestHandler(...)` | Addressed to the Entry Spot's Actor |
| `zlinkHandler(...)` | For specifying the branch above directly |

**The packet name must match the sending side exactly.** Share it through a constants
module.

## 3. Registration Surface

| Name | What it does |
| --- | --- |
| `ZLinkModule.forRootFactory({ useFactory })` | The registration entry point. The factory returns a builder |
| `zlinkFramework()` | Creates the builder |
| `zlinkModule(__dirname, options)` | Collects that directory's handlers/Spots/Actors as providers |
| `zlinkDiscoverProviders(...)` | For controlling provider discovery directly |
| `ZLinkFrameworkOptionsBuilder` | The builder type |
| `ZLinkMeshNodeBuilder` · `ZLinkMeshChannelBuilder` · `ZLinkMeshObjectRoleBuilder` | MeshNode and its roles |
| `ZLinkFanoutChannelBuilder` · `ZLinkStreamNodeBuilder` | Fanout channel / STREAM node |
| `ZLinkMeshPeerConnections` · `ZLinkEndpointConnections` | Manual peer connections |
| `ZLinkMeshNodeSocketConfig` | Socket caps |

## 4. Spot And Actor

| Contract | Nature |
| --- | --- |
| `ZLinkSpot` · `ZLinkEntrySpot<TActor>` · `ZLinkInstanceSpot` | You implement it |
| `ZLinkSpotContext` · `ZLinkEntrySpotContext` · `ZLinkInstanceSpotContext` | Received as `readonly context` |
| `ZLinkSpotCommonContext` | The part shared by the three above |
| `ZLinkSpotManager` · `ZLinkSpotCreateResult` · `ZLinkSpotCreateState` | Creation and its result |
| `ZLinkSpotCreateResponse` · `ZLinkSpotActorJoinResult` | Admission responses |
| `ZLinkSpotRelocationAdapter` · `ZLinkSpotRelocationReadyCall` | State relocation |
| `ZLinkTimer` · `ZLinkTimerOptions` · `ZLinkTimerTick` | Timer |
| `ZLinkWorkerCall<T>` | The return of `runCpuWorker` / `runIoWorker` |
| `ZLinkActor` · `ZLinkActorContext` · `ZLinkActorManager` | Actor |
| `ZLinkActorCreateResult` · `ZLinkActorJoinCompletion` | Creation / join results |
| `ZLinkActorRelocationAdapter` | Actor state relocation |

**A Spot/Actor receives its context as `readonly context!`.** It's not a constructor
argument — it's a property the framework fills in.

**Result types are discriminated unions.** Split them with a discriminant like
`result.status === 'rejected'`. This corresponds to a sealed class or `std::variant` in the
other languages.

## 5. STREAM Session

| Contract | Nature |
| --- | --- |
| `ZLinkSession` | You implement it |
| `ZLinkSessionContext` · `ZLinkSessionDispatchContext` | Context |
| `ZLinkSessionActor` · `ZLinkSessionActors` | The bound Actor |
| `ZLinkStreamError` | Error notification |
| `ZLinkBoundSession` | Pushing to a session bound to an Actor |

## 6. Observation And Failure

| Contract | Nature |
| --- | --- |
| `ZLinkFrameworkRuntime` · `ZLinkFrameworkRuntimeStatus` | Host status |
| `ZLinkRouteMeshRuntime` · `ZLinkRouteMeshStatus` | MeshNode status |
| `ZLinkDispatchOptionsBuilder` · `ZLinkMessageFlowLogMode` | Diagnostics level |
| `ZLinkMessageFlowObserver` | Flow records |
| `ZLinkFrameworkException` | A failure. `kind` / `isRetriable` |
| `ZLinkFrameworkErrorKind` | The failure branch |

## 7. Where Node Differs

Three spots that trip up readers coming from another language.

| Spot | Node |
| --- | --- |
| The timeout argument | **A number in milliseconds**, not a `Duration` — `timeout(3_000)` |
| Passing cancellation | An **`AbortSignal`**, not a `CancellationToken` |
| Discriminating a result | A **discriminant property** — `result.status` — not a type check |

## 8. Where They Come From

| How you get it | Surface |
| --- | --- |
| You implement it | `ZLinkSpot` · `ZLinkEntrySpot` · `ZLinkActor` · `ZLinkSession` · the `*Handler` contracts |
| Received as `readonly context!` | The `ZLinkSpotContext` family · `ZLinkActorContext` · `ZLinkSessionContext` |
| Received via `@Inject(token)` | Every token in §1 |
| The `zlinkFramework()` builder returns it | The `ZLinkMeshNodeBuilder` family |
| A call returns it | `*Call` · `*Result` · `*Status` |

## 9. Related Documents

- Exact signatures: [Node.js exact interface table of contents](../../../common/spec/server/languages/node/interfaces/README.ko.md)
- Registration entry point: [2. Getting Started](02-getting-started.en.md)
- The NestJS host contract: [Node.js NestJS host public contract](../../../common/spec/server/languages/node/interfaces/07-nestjs-host.en.md)
