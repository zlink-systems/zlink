---
title: "13. Key Type Usage Index · Java"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: Operations — metrics · drain · readiness](12-operations.en.md) | [Next: Picking A Sample](14-samples.en.md)
<!-- framework-adapter-nav:end -->

# 13. Key Type Usage Index

> **The document that owns this chapter's contract** —
> the [Java exact interface table of contents](../../../common/spec/server/languages/java/interfaces/README.ko.md)
> owns the exact signatures. This chapter is a guide to finding the public interfaces an
> application uses often, organized by feature.

Java surfaces read fastest split by **how you get them.** Four groups: injected as a bean,
implemented directly, returned by a startup-phase builder, and returned by a call.

## 1. Channel Messaging

The calling side gets the client injected via constructor.

```java
@Service
public class OrderService {
    private final ZLinkRouteClient client;

    public OrderService(ZLinkRouteClient client) {
        this.client = client;
    }

    public CompletionStage<OrderPlaced> place(PlaceOrder request) {
        return client
            .requestToChannel("orders", request)
            .timeout(Duration.ofSeconds(3))
            .submit(OrderPlaced.class);
    }
}
```

| Interface | What the application does with it |
| --- | --- |
| `ZLinkRouteClient` | send / request by ChannelName or a managed Node RID |
| `ZLinkFanoutClient` | Publish an event to a classic fanout channel |
| `ZLinkSpotPublisherClient` | Publish a Logical Multicast from outside a Spot |
| `ZLinkSendCall` · `ZLinkRequestCall` · `ZLinkPublishCall` | Each call's timeout / metadata / terminal |
| `ZLinkMessageContext` · `ZLinkRouteMessageContext` | This dispatch's metadata and origin |
| `ZLinkMessage` | A payload not yet decoded |

The receiving side implements a handler interface.

| Handler interface | What it receives |
| --- | --- |
| `ZLinkRequestHandler<TReq, TRes>` | A channel request |
| `ZLinkSendHandler<TMsg>` | A channel one-way send |
| `ZLinkFanoutHandler<TEvent>` | A classic fanout event |
| `ZLinkRouteRequestHandler` · `ZLinkRouteSendHandler` | Node direct |

**There's also an attribute-based way to group them.** Group a class with
`@ZLinkHandlerGroup` and put `@ZLinkRequest` / `@ZLinkSend` / `@ZLinkPublish` on methods,
and it registers without implementing an interface. Registration decides which channel it's
exposed on.

## 2. Topology Registration

Builders under the `ZLinkFrameworkOptions` that `ZLinkFrameworkConfigurer` receives. None of
these are usable after the Spring context starts.

| Interface | What it registers |
| --- | --- |
| `ZLinkFrameworkOptions` | The root — codec, handler discovery, location store, dispatch |
| `ZLinkMeshNodeBuilder` | A single MeshNode (`addRouteMesh`) |
| `ZLinkMeshChannelBuilder` → `...ServerBuilder` · `...ClientBuilder` | That node's channel role |
| `ZLinkMeshObjectRoleBuilder` → `...ServerBuilder` · `...ClientBuilder` | Object role and Spot/Actor registration |
| `FanoutChannelBuilder` | A classic fanout channel |
| `ClientServerChannelBuilder` | A client/server channel pair |
| `ZLinkStreamNodeBuilder` | A STREAM node |
| `ZLinkMeshPeerConnections` · `ZLinkEndpointConnections` | Manual peer connections |
| `ZLinkMeshNodeSocketConfig` | Socket caps ([16. Options](16-options.en.md) §3.1) |
| `ZLinkUserSpotFactoryBuilder` · `ZLinkInstanceSpotFactoryBuilder` · `ZLinkActorFactoryBuilder` | Policy at registration |
| `ZLinkCodecRegistryBuilder` · `ZLinkCodecExtension` | Serialization format |
| `ZLinkMetadataPolicyBuilder` | Metadata propagation policy |

## 3. Spot

| Interface | Nature |
| --- | --- |
| `ZLinkSpot` · `ZLinkSpot<TActor>` | User Spot — you implement it |
| `ZLinkEntrySpot<TActor>` | Entry Spot — you implement it |
| `ZLinkInstanceSpot` | Instance Spot — you implement it |
| `ZLinkSpotContext` · `ZLinkEntrySpotContext` · `ZLinkInstanceSpotContext` | Received via constructor |
| `ZLinkSpotManager` | Injected; creates and finds Spots |
| `ZLinkSpotCreateCall` · `ZLinkSpotGetOrCreateCall` | The create call |
| `ZLinkSpotCreateResult` · `ZLinkSpotCreateState` | The create result and its three states |
| `ZLinkSpotCreateResponse` | A creation callback's accept / reject |
| `ZLinkSpotActorJoinResult` | A join admission's accept / reject |
| `ZLinkSpotClosingContext` · `ZLinkSpotCloseReason` | The deadline and reason while closing |
| `ZLinkSpotHandlerRegistry` · `ZLinkInstanceSpotHandlerRegistry` | Registers handlers inside `configure()` |
| `ZLinkSpotOutbound` | Calls going out from a Spot |
| `ZLinkSpotRelocationAdapter<TSpot>` | The adapter that packs and unpacks state |
| `ZLinkSpotRelocationReadyCall` · `...Completion` · `...Outcome` | The relocation-ready signal and its result |
| `ZLinkSpotSendCall` · `ZLinkSpotRequestCall` | Calls targeting a Spot |

A Spot receives four kinds of handler.

| Handler interface | What it receives |
| --- | --- |
| `ZLinkSpotPacketHandler<TSpot, TMsg>` | A one-way packet addressed to the Spot |
| `ZLinkSpotRequestHandler<TSpot, TReq, TRes>` | A request addressed to the Spot |
| `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | A Logical Multicast subscription event |
| `ZLinkSpotTimerHandler<TSpot>` | A timer tick |
| `ZLinkSpotActorSendHandler` · `ZLinkSpotActorRequestHandler` | A packet / request addressed to a member Actor |

Timer-related types are `ZLinkTimer` (a cancel handle), `ZLinkTimerOptions`,
`ZLinkTimerOverrunPolicy`, and `ZLinkTimerTick`.

Worker-related types are `ZLinkWorkerCall<T>`, `ZLinkWorkerTask<T>` (sync),
`ZLinkIoWorkerTask<T>` (async), and `ZLinkWorkerCancellation`; failures split into
`ZLinkWorkerQueueFullException`, `ZLinkWorkerTimeoutException`, and
`ZLinkWorkerFailedException`.

## 4. Actor

| Interface | Nature |
| --- | --- |
| `ZLinkActor` | You implement it |
| `ZLinkActorContext` | Received via constructor. Join / bound-session access |
| `ZLinkActorManager` | Injected; creates and finds Actors |
| `ZLinkActorClient` | send / request by ActorId |
| `ZLinkActorFactory` | Creation method |
| `ZLinkActorCreateCall` · `ZLinkActorGetOrCreateCall` | The create call |
| `ZLinkActorCreateResult` | The creation result -- `Existing` / `Created` / `Rejected` |
| `ZLinkActorCreateResponse` | The Entry Spot's admission response |
| `ZLinkActorJoinCall` · `ZLinkActorJoinCompletion` · `ZLinkActorJoinOperationId` | Reserving and completing a join |
| `ZLinkActorRelocationAdapter<TActor>` · `ZLinkRelocationCancellation` | State relocation |
| `ZLinkActorHandlerRegistry` | Registers Actor handlers |
| `ZLinkBoundSession` · `ZLinkBoundSessionSendCall` | Pushing to a bound session |

**The creation result and join completion are sealed hierarchies.** Split them with
`instanceof` pattern matching or `switch`.

## 5. STREAM Session

| Interface | Nature |
| --- | --- |
| `ZLinkSession` | You implement it. `configure` / `onDispatch` / lifecycle callbacks |
| `ZLinkSessionContext` | Received via constructor |
| `ZLinkSessionDispatchContext` | This packet's dispatch info |
| `ZLinkSessionClient` | reply / send |
| `ZLinkSessionReplyCall` · `ZLinkSessionSendCall` | Each call |
| `ZLinkSessionActor` · `ZLinkSessionActors` | An Actor bound to the session |
| `ZLinkSessionPacketDispatcher` · `ZLinkTypedSessionPacketHandler` | Typed packet processing |
| `ZLinkStreamError` · `ZLinkStreamSessionError` | Error notification |
| `ZLinkStreamCodec` · `ZLinkStreamCompressionCodec` | Encoding and compression |

## 6. Location And Relocation

| Interface | Nature |
| --- | --- |
| `ZLinkLocationStore` · `ZLinkRelocationStore` | Implement directly or use a provided implementation |
| `ZLinkRedisLocationStore` · `ZLinkRedisLocationOptions` | The Redis implementation and its settings |
| `ZLinkRedisRelocationStore` · `ZLinkRedisRelocationOptions` | Same, for relocation |
| `ZLinkLocationOptions` | Behavior values ([16. Options](16-options.en.md) §5) |
| `ZLinkLocationReadiness` | Whether the required peers are Ready |
| `ZLinkLocationRuntimeQuery` | Status and topology queries |

Implementing a store yourself is rare. You'll only look at the `ZLinkStore*` /
`ZLinkBlob*` family then.

## 7. Host And Observation

| Interface | Nature |
| --- | --- |
| `ZLinkFrameworkRuntime` | Host status, plus relocate / shutdown |
| `ZLinkFrameworkRuntimeStatus` · `ZLinkFrameworkRuntimeState` | The status record and its state value |
| `ZLinkRouteMeshRuntime` | A MeshNode snapshot and observation |
| `ZLinkRouteMeshRuntimeOptions` | Adjusting weight while running |
| `ZLinkClientServerRuntime` · `ZLinkFanoutRuntime` | That channel's status |
| `ZLinkMeshNodeSnapshot` · `ZLinkMeshPeerSnapshot` · `ZLinkMeshChannelSnapshot` | Snapshot records |
| `ZLinkDispatchOptions` · `ZLinkDiagnosticsOptions` | Diagnostics level |
| `ZLinkMessageFlowObserver` · `ZLinkMessageFlowEvent` | Message flow records |
| `ZLinkMetricsCustomizer` | Adjusting the Micrometer registry |

## 8. Failure Types

| Exception | When |
| --- | --- |
| `ZLinkConfigurationException` | Registration is invalid. Thrown at context startup |
| `ZLinkFrameworkException` | A runtime failure. Split by `kind()` / `retriable()` |
| `ZLinkRequestFailureException` | A request ended in failure. Carries a `ZLinkRequestFailureReason` |
| `ZLinkOperationCanceledException` | Canceled |
| `ZLinkWorkerQueueFullException` and others | The three-way worker failure |

`ZLinkFrameworkErrorKind` is the enum holding the failure branch.

## 9. Where They Come From

| How you get it | Interfaces |
| --- | --- |
| You implement it | `ZLinkSpot` · `ZLinkEntrySpot` · `ZLinkInstanceSpot` · `ZLinkActor` · `ZLinkSession` · the `*Handler` family |
| Received via constructor (context) | The `ZLinkSpotContext` family · `ZLinkActorContext` · `ZLinkSessionContext` |
| Injected as a bean | `ZLinkRouteClient` · `ZLinkFanoutClient` · `ZLinkActorClient` · `ZLinkSpotManager` · `ZLinkActorManager` · `ZLinkFrameworkRuntime` · `ZLinkRouteMeshRuntime` · `ZLinkLocationRuntimeQuery` |
| A startup-phase builder returns it | The `ZLinkMeshNodeBuilder` family · `ZLinkStreamNodeBuilder` · `FanoutChannelBuilder` |
| A call returns it | `*Call` · `*Result` · `*Response` · `*Snapshot` |

**Handlers and Spot/Actor/Session aren't beans.** The framework creates them, and only their
constructor arguments get injected from the Spring container ([2. Getting Started](02-getting-started.en.md) §3).

## 10. Related Documents

- Exact signatures: [Java exact interface table of contents](../../../common/spec/server/languages/java/interfaces/README.ko.md)
- Registration entry point: [2. Getting Started](02-getting-started.en.md)
- Options and defaults: [16. Options](16-options.en.md)
- Observation surfaces: [11. Monitoring](11-monitoring.en.md)
