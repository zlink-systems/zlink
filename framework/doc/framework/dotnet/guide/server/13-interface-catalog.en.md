---
title: "13. Key Type Usage Index · C#/.NET"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.en.md) | [Previous: Operations — metrics · drain · readiness](12-operations.en.md) | [Next: Picking A Sample](14-samples.en.md)
<!-- framework-adapter-nav:end -->

# 13. Key Type Usage Index

> **The document that owns this chapter's contract** —
> [.NET exact interface](../../../common/spec/server/languages/dotnet/interfaces/README.ko.md)
> owns the exact signatures. This chapter is a guide for finding the public interfaces an
> application uses most often, by feature.

## 1. Channel Messaging

`IZLinkRouteClient` selects one ready server by ChannelName.

```csharp
await routeClient
    .SendToChannel("game.api", new PlayerOnline("player-1"))
    .Async(ct); // Waits only for source-local outbound admission.

var reply = await routeClient
    .RequestToChannel("game.api", new GetPlayer("player-1"))
    .Timeout(TimeSpan.FromSeconds(3))
    .Async<Player>(ct); // Waits for the selected handler's reply.
```

| Interface | What the application does with it |
|---|---|
| `IZLinkRouteClient` | Send/request by ChannelName or a managed Node RID |
| `IZLinkSendCall` | Submits a one-way operation |
| `IZLinkRequestCall` | Sets a timeout and receives a typed reply |
| `IZLinkFanoutClient` | Publishes an event to a classic fanout channel |

Node direct is used only when managing a specific MeshNode itself. Business-object placement
or messaging uses ActorId, SpotId, or ChannelName.

```csharp
var status = await routeClient
    .RequestToNode(
        "play",
        RoutingId.From("play-node-1"),
        new GetNodeStatus())
    .Async<NodeStatus>(ct); // An operational system queries a specific node's status.
```

For the exact handler and call interfaces, see the
[Channel messaging exact interface](../../../common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md).

## 2. Topology Registration

A MeshNode's Object role and RouteMesh Channel role are registered independently.

```csharp
services.AddZLinkFramework(options =>
{
    var play = options.AddRouteMesh("play")
        .Listen(5501)
        .SetRoutingIdPrefix("play")
        .SetPlacementWeight(100);

    play.Objects().Server()
        .AddSpotFactory<RoomSpot>(
            "room",
            factory => factory
                .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                .PreserveStateWith<RoomRelocationAdapter>())
        .AddActorFactory<PlayerActor, PlayerActorFactory>(
            "player",
            factory => factory
                .PreserveStateWith<PlayerRelocationAdapter>());

    play.Channel("play.api").Server()
        .SetWeight(100)
        .AddRequestHandler<GetPlayerHandler, GetPlayer, Player>();
});
```

| Interface | What the application does with it |
|---|---|
| `IZLinkFrameworkOptions` | Registers the Store, topology, handlers, and common options |
| `IZLinkMeshNodeBuilder` | Registers the RouteMesh socket, Node RID, placement, and role |
| `IZLinkMeshObjectRoleBuilder` | Registers the Object Client or Server capability |
| `IZLinkMeshObjectServerBuilder` | Registers the Entry Spot and stable Actor/Spot types |
| `IZLinkMeshChannelRoleBuilder` | Registers the RouteMesh Channel Client or Server membership |
| `IZLinkClientServerChannelRoleBuilder` | Registers the ClientServer Client/Server role |
| `IZLinkFanoutChannelBuilder` | Registers a classic fanout publisher/subscriber |
| `IZLinkStreamNodeBuilder` | Registers a STREAM listener and session |

The Entry Spot's SpotId is issued by the Framework in the form `<prefix>-entry-<uuid>`.
There's no API for the application to set the Entry Spot's RoutingId or SpotId.

For the exact builders, see the
[Topology exact interface](../../../common/spec/server/languages/dotnet/interfaces/03-configuration-topology.en.md).

## 3. Spot

A User Spot is created through the manager. The application never specifies the target Node
RID.

```csharp
ZLinkSpotCreateResult created = await spotManager
    .Create("room")
    .InMesh("play")
    .Request(new CreateRoom("ranked"))
    .Async(ct); // The Framework selects the global SpotId and an eligible target.

ZLinkSpotCreateResult existingOrCreated = await spotManager
    .GetOrCreate("lobby-eu", "lobby")
    .InMesh("play")
    .Request(new CreateLobby("eu"))
    .Async(ct);
```

Ordinary Spot messaging uses only the global SpotId.

```csharp
await spotClient
    .SendToSpot("room-42", new RoundStarted())
    .Async(ct);

var state = await spotClient
    .RequestToSpot("room-42", new GetRoomState())
    .Async<RoomState>(ct);
```

An Instance Spot has no separate create API. State the activation intent on the first
message sent to a missing Spot.

```csharp
var match = await spotClient
    .RequestToSpot("matchmaking:gold", new FindMatch("player-1"))
    .InstanceSpot("level-matchmaking")
    .InMesh("matchmaking")
    .Async<MatchFound>(ct);
```

| Interface | What the application does with it |
|---|---|
| `IZLinkSpotManager` | User Spot create, get-or-create, current-ref lookup, and exact close |
| `IZLinkSpotClient` | Spot send/request by global SpotId |
| `IZLinkSpotOutbound` | Spot/Channel/Logical Multicast calls from inside a Spot callback |
| `IZLinkSpotContext` | Manages handlers, timers, workers, close, and the relocation-ready turn |
| `IZLinkInstanceSpotContext` | Manages Instance Spot handlers, timers, workers, and close |
| `IZLinkEntrySpotContext` | Manages Entry Spot handlers, timers, and the Actor lifecycle |
| `IZLinkSpotRelocationAdapter<TSpot>` | Captures/restores opaque state bytes in `PreserveStateWith` |
| `IZLinkSpotPacketHandler<TSpot, TMessage>` | Handles a one-way packet addressed to the Spot |
| `IZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | Handles a request addressed to the Spot and returns a reply |
| `IZLinkSpotSubscriptionHandler<TSpot, TEvent>` | Handles a Logical Multicast subscription event |
| `IZLinkSpotTimerHandler<TSpot>` | Handles a Spot timer tick |

`SpotRef` is a snapshot of the current location. It isn't held onto as an ordinary message
target. Use it for an operation that needs to confirm the exact generation, like
`CloseAsync(spotRef)`.

For the exact lifecycle and calls, see the
[Spot exact interface](../../../common/spec/server/languages/dotnet/interfaces/05-spots.en.md).

## 4. Actor

An Actor is also created and called by global ActorId.

```csharp
ZLinkActorCreateResult result = await actorManager
    .GetOrCreate("player-1", "player")
    .InMesh("play")
    .Request(new CreatePlayer("player-1"))
    .Async(ct);

await actorClient
    .SendToActor("player-1", new GrantReward("daily"))
    .Async(ct);
```

To schedule a User Spot join from inside an Actor handler, use the deferred call, which
doesn't block the current turn.

```csharp
actor.Context
    .JoinSpot("room-42", new JoinRoom("player-1"))
    .Timeout(TimeSpan.FromSeconds(3))
    .Defer(); // Executes on the Actor queue, in order, once the current handler finishes.
```

| Interface | What the application does with it |
|---|---|
| `IZLinkActorManager` | Actor create, get-or-create, current-ref/Spot lookup, and exact destroy |
| `IZLinkActorClient` | Actor send/request by global ActorId |
| `IZLinkActorContext` | The current Actor identity, Spot membership, session binding, and deferred join |
| `IZLinkActorFactory<TActor>` | Creates an Actor instance on the target the Framework selected |
| `IZLinkActorRelocationAdapter<TActor>` | Captures/restores opaque state bytes in `PreserveStateWith` |
| `IZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | Handles a one-way packet addressed to a member Actor |
| `IZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | Handles a request addressed to a member Actor and returns a reply |

`ActorRef` is also a snapshot pointing at an exact incarnation. Ordinary messaging uses the
ActorId.

For the exact interfaces, see the
[Actor exact interface](../../../common/spec/server/languages/dotnet/interfaces/06-actors.ko.md).

## 5. STREAM Session

A session receives a client connection and registers typed handlers. Once bound to an
Actor, the Actor can push to the current session.

```csharp
public sealed class GatewaySession(IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddHandler<AuthenticateHandler>();
        // Wires an incoming packet to a typed handler.
    }

    public ValueTask OnConnectedAsync(CancellationToken ct)
        => ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken ct)
        => ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken ct)
        => ValueTask.CompletedTask;
}
```

| Interface | What the application does with it |
|---|---|
| `IZLinkSession` | The STREAM connection lifecycle and handler registration |
| `IZLinkSessionContext` | Session identity, client, Actor binding, and close |
| `IZLinkSessionClient` | Send or reply to a request, to the connected client |
| `IZLinkSessionActors` | Binds an ActorRef to the current session |
| `IZLinkBoundSession` | Pushes from an Actor to the bound session |

For the exact interfaces, see
[STREAM](../../../common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md) and
[Bound session](../../../common/spec/server/languages/dotnet/interfaces/07-bound-stream-session.ko.md).

## 6. Location And Relocation

Register the two Store capabilities separately.

```csharp
options.AddLocationStore(
    new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
    {
        ConnectionString = "redis:6379",
        KeyPrefix = "game:location"
    }));

options.AddRelocationStore(
    new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
    {
        ConnectionString = "redis:6379",
        KeyPrefix = "game:relocation"
    }));
```

| Interface | Responsibility |
|---|---|
| `IZLinkLocationStore` | Read/write/atomic-batch of the opaque location record the Framework passes in |
| `IZLinkRelocationStore` | Put/get/delete of the immutable relocation blob the Framework passes in |
| `IZLinkLocationReadiness` | Confirms whether a needed Mesh peer is Ready |
| `IZLinkLocationRuntimeQuery` | Queries Location health and paged topology/service summary |

The Provider SPI is public, but an application developer never calls it directly. A
provider implementer only implements the two deep interfaces — the authority record,
reservation, aggregate, and recovery state machine are all managed by the Framework.

## 7. Host And Topology Observation

Host relocation and shutdown are owned by `IZLinkFrameworkRuntime`.

```csharp
var result = await runtime.RelocateAsync(
    new ZLinkFrameworkRelocationOptions
    {
        Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
        Deadline = TimeSpan.FromSeconds(30)
    },
    ct);

if (result.Outcome == ZLinkFrameworkRelocationOutcome.Relocated)
{
    await runtime.ShutdownAsync(TimeSpan.FromSeconds(10), ct);
}
```

| Interface | What the application does with it |
|---|---|
| `IZLinkFrameworkRuntime` | Host status, Relocate, Shutdown, and the status stream |
| `IZLinkRouteMeshRuntime` | Current status and status stream per RouteMesh |
| `IZLinkClientServerRuntime` | Current status and status stream per ClientServer channel |
| `IZLinkFanoutRuntime` | Current status and status stream per fanout channel |
| `IZLinkDiagnosticsRuntime` | Changes the diagnostics level and sampling while running |

Public monitoring provides only status the application can act on. Socket generation,
authority records, relocation staging, and internal mailbox state are left in
logs/traces or the Framework's internal diagnostics.

## 8. Related Documents

- [Public contract governance principles](../../../common/spec/00-public-contract-governance.ko.md)
- [.NET exact interface index](../../../common/spec/server/languages/dotnet/interfaces/README.ko.md)

---
<!-- framework-adapter-nav:bottom:start -->
[Guide Home](../../../index.en.md) | [Previous: Operations — metrics · drain · readiness](12-operations.en.md) | [Next: Picking A Sample](14-samples.en.md)
<!-- framework-adapter-nav:bottom:end -->
