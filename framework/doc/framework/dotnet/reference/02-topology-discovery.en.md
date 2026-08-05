# 02. Topology discovery

[Reference index](README.en.md)

This category covers the topology registration entry points `IZLinkFrameworkOptions` provides,
and the entry points that query RouteMesh·ClientServer·Fanout operational status. The exact
signatures are owned by the
[RouteMesh·MeshNode exact interface](../../common/spec/server/languages/dotnet/interfaces/03-configuration-topology.en.md)
and the
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.en.md)
(both Korean-only). Every registration entry is a host configuration-time call.

---

## `AddRouteMesh` (configuration time)

Registers one physical MeshNode. It is the starting point for RouteMesh-based topology.

```csharp
services.AddZLinkFramework(options =>
{
    var play = options.AddRouteMesh("play")
        .Listen(5501)
        .SetRoutingIdPrefix("play")
        .SetPlacementWeight(100);
});
```

**Options.** Commonly used modifiers:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Listen(endpoint)` / `.Listen(port)` | does not bind if omitted | this MeshNode's inbound endpoint. It is fine to skip listening if Server membership is zero |
| `.SetBindHost(string)` / `.SetAdvertiseHost(string)` | follows `ConfigureNetwork()`'s root default | the bind·advertise host that applies only to this MeshNode. It takes priority over the root default |
| `.SetRoutingId(RoutingId)` / `.SetRoutingIdPrefix(string)` | Framework-issued | a fixed RID, or the prefix for an issued RID |
| `.SetPlacementWeight(int)` | 100 (range `0..10000`) | relative weight for placing new Actors·Spots on this node |
| `.SetActorLimit(int)` / `.SetSpotLimit(int)` | `0` (unlimited) | the Actor·Spot cap this node accepts |
| `.SetActivationConcurrency(int)` | 128 | the cap on activation admissions in progress concurrently — not object population |
| `.SetDefaultRequestTimeout(TimeSpan)` | this MeshNode's default request timeout | the value `RequestToNode`/`RequestToChannel` (messaging-execution category) etc. use when `.Timeout(...)` is omitted |
| `.SetInstanceSpotIdleTimeout(TimeSpan)` | `TimeSpan.Zero` (no reclamation) | idle reclamation time for Instance Spots. The valid range is `TimeSpan.Zero` or greater; a negative value is a startup error |
| `.Objects()` | — | enters Object role (Client/Server) registration. See the Object role registration entry |
| `.Channel(channelName)` | — | enters this MeshNode's RouteMesh Channel role registration. See the RouteMesh Channel registration entry |
| `.AddRouteSendHandler<THandler, TMessage>(packetName?)` | packet name is derived from the message type | registers a Node direct one-way handler (`IZLinkRouteSendHandler<TMessage>`) — the target `SendToNode` (messaging-execution category) calls |
| `.AddRouteRequestHandler<THandler, TRequest, TReply>(packetName?)` | packet name is derived from the message type | registers a Node direct request handler (`IZLinkRouteRequestHandler<TRequest, TReply>`) — the target `RequestToNode` calls |

**Completion.** Registers synchronously with no return value. An invalid combination
(duplicate MeshName, a missing listener setting, etc.) surfaces as
`ZLinkConfigurationException` during host startup validation. The same packet name can be
registered separately in the RouteMesh Channel handler family and the Node direct handler
family — only a duplicate key within the same family is a startup error.

**When to use it.** Every host that uses RouteMesh registers at least one MeshNode. A node
that only uses manual peers and needs no distributed discovery can start without a Location
Store.

---

## Object role registration (configuration time)

Registers how a MeshNode handles Actors·Spots — whether it is Client-only or hosts them as a
Server.

```csharp
play.Objects().Server()
    .AddEntrySpot<GameEntrySpot>()
    .AddSpotFactory<RoomSpot>(
        "room",
        factory => factory
            .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
            .PreserveStateWith<RoomRelocationAdapter>())
    .AddActorFactory<PlayerActor, PlayerActorFactory>(
        "player",
        factory => factory
            .PreserveStateWith<PlayerRelocationAdapter>());
```

**Options.** Commonly used modifiers:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.AddEntrySpot<TEntrySpot>()` | none | registers the Entry Spot type dedicated to external entry |
| `.AddSpotFactory<TSpot>(type, configure)` | none | registers a stable User Spot type. `configure` takes `StableTypeLimit(int)`·`ExecutionMode`·`RelocationReadiness`, plus exactly one of `PreserveStateWith`/`RecreateOnRelocation`/`DisableRelocation` |
| `.AddInstanceSpotFactory<TSpot>(type, configure)` | none | registers a cold-activation Instance Spot type. `configure` takes `StableTypeLimit(int)`, plus exactly one of `PreserveStateWith`/`RecreateOnRelocation`/`DisableRelocation` |
| `.AddActorFactory<TActor, TFactory>(type, configure)` | none | registers a stable Actor type. `configure` takes exactly one of `PreserveStateWith`/`RecreateOnRelocation`/`DisableRelocation` (the Actor factory has no `StableTypeLimit`) |

**Completion.** Registers synchronously with no return value. An adapter/factory mismatch for a
stable type using relocation, or a duplicate type, surfaces as `ZLinkConfigurationException`
during host startup validation.

**When to use it.** Register the corresponding role when this node actually hosts (Server) an
Actor·Spot, or only references an Actor·Spot hosted by another node as a messaging target
(Client). See the actor-relocation category for the relocation-policy selection criteria.

---

## RouteMesh Channel registration (configuration time)

Registers logical ChannelName membership within the same MeshNode.

```csharp
play.Channel("play.api").Server()
    .SetWeight(100)
    .AddRequestHandler<GetPlayerHandler, GetPlayer, Player>()
    .AddSendHandler<PlayerOnlineHandler, PlayerOnline>();
```

**Options.** After `Channel(channelName)`, call exactly one of `.Client()` or `.Server()`.
`.Client()` only creates the send path and has no modifiers. `.Server()`'s commonly used
modifiers:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.SetWeight(int)` | 100 (range `0..10000`) | relative weight for this Server being selected as a request/send target. `0` excludes it from selection |
| `.AddHandlerGroup(groupName)` | none | specifies the handler group assembly scanning looks for |
| `.AddSendHandler<THandler, TMessage>(packetName?)` | packet name is derived from the message type | registers a one-way handler |
| `.AddRequestHandler<THandler, TRequest, TReply>(packetName?)` | packet name is derived from the message type | registers a request/reply handler |

**Completion.** Registers synchronously with no return value. A duplicate handler key within
the same owner namespace surfaces as `ZLinkConfigurationException` during host startup
validation.

**When to use it.** Use `.Server()` to register the handler that `SendToChannel`/`RequestToChannel`
(messaging-execution category) will reach. If this MeshNode only calls another node's Server and
places no handler of its own, register `.Client()` alone. For messaging between separate
processes, use ClientServer Channel registration instead.

---

## `AddClientServerChannel` (configuration time)

Registers an independent ClientServer Channel unrelated to RouteMesh.

```csharp
options.AddClientServerChannel("payments.api").Server()
    .Listen(6001)
    .SetWeight(100)
    .AddRequestHandler<ChargeHandler, Charge, ChargeResult>();

options.AddClientServerChannel("payments.api").Client()
    .Connect("payments-1:6001");
```

**Options.** Commonly used modifiers:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Server().Listen(port)` | `0` if the port is omitted (automatic discovery auto-binds whether the port is omitted or `0`) | this Server's inbound port. There is no `Listen(string endpoint)` overload — it takes only an int port, not an endpoint string |
| `.Server().SetBindHost(string)` / `.SetAdvertiseHost(string)` | follows the root `ConfigureNetwork()` default | the bind·advertise host that applies only to this Server |
| `.Server().SetWeight(int)` / `.AddSendHandler`/`.AddRequestHandler` | same as RouteMesh Channel Server | weight and handler registration |
| `.Client().Connect(endpoint)` | manual | manually connects to a specific Server. Omitting it lets automatic discovery find the target |

**Completion.** Registers synchronously with no return value. A Client·Server using automatic
discovery without a registered Location Store surfaces as `ZLinkConfigurationException` during
host startup validation.

**When to use it.** Use it for request/reply or one-way messaging between independent services
that are not RouteMesh members. Between nodes in the same RouteMesh, use RouteMesh Channel
registration instead.

---

## `AddFanoutChannel` (configuration time)

Registers a classic fanout-only channel — the target `Publish` (messaging-execution category)
publishes to.

```csharp
options.AddFanoutChannel("lobby.events")
    .EnablePublisher(7001)
    .AddHandler<PlayerJoinedHandler, PlayerJoined>();

// automatic subscriber — discovers every publisher of the same ChannelName through the Location Store
options.AddFanoutChannel("lobby.events")
    .EnableSubscriber();

// manual subscriber — uses only the specified endpoint. Combining it with EnableSubscriber() fails startup
options.AddFanoutChannel("lobby.events")
    .Connect("lobby-1:7001");
```

**Options.** Commonly used modifiers:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.EnablePublisher(endpoint)` / `.EnablePublisher(port)` | none | registers this channel's publisher role and inbound endpoint |
| `.SetBindHost(string)` / `.SetAdvertiseHost(string)` / `.SetRoutingId(RoutingId)` / `.SetRoutingIdPrefix(string)` | the root default, or Framework-issued | the bind·advertise host and RID that apply only to the publisher |
| `.EnableSubscriber()` | — | an automatic subscriber. Discovers every valid publisher of the same ChannelName through the Location Store |
| `.Connect(endpoint)` | — | a manual subscriber. Uses only the specified endpoint. Registering it alongside `.EnableSubscriber()` on the same channel fails host startup |
| `.AddHandler<THandler, TEvent>(packetName?)` | packet name is derived from the event type | registers a typed event handler |

**Completion.** Registers synchronously with no return value. Configuring an automatic
subscriber and a manual subscriber together on the same fanout channel surfaces as
`ZLinkConfigurationException`.

**When to use it.** Use it to create a new observation/notification channel where the
publisher does not need to know its subscribers. For messaging that needs a reply, use
RouteMesh Channel or ClientServer Channel registration instead.

---

## `AddStreamNode` (configuration time)

Registers a listener that accepts external STREAM connections.

```csharp
options.AddStreamNode("public-gateway")
    .Bind(9001)
    .EnableActorDispatch()
    .AddSession<GameSession>();
```

**Options.** Commonly used modifiers:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Bind(endpoint)` / `.Bind(port)` | `0` if the port is omitted (automatic discovery auto-binds whether the port is omitted or `0`) | this STREAM listener's inbound port. A `Bind(string endpoint)` overload also exists |
| `.SetBindHost(string)` / `.SetAdvertiseHost(string)` | follows the root `ConfigureNetwork()` default | the bind·advertise host that applies only to this listener |
| `.ConfigureSocket()` | socket defaults | individual tuning of this listener socket's `MaxMessageSize`·HWM·buffer·timeout etc. (`IZLinkSocketConfig`) |
| `.EnableActorDispatch()` | disabled | dispatches incoming messages to the Actor bound to the Session |
| `.SetTlsServer(certPath, keyPath, requireClientCertificate?)` | no TLS | the TLS server certificate/key and whether mutual authentication is required |
| `.AddSession<TSession>()` | none | registers the Session type to create per connection |

**Completion.** Registers synchronously with no return value. A TLS configuration error
surfaces as `ZLinkConfigurationException` during host startup validation.

**When to use it.** Use it to open a gateway that external clients connect to directly with
the STREAM protocol. See the stream-session category for the exact Session·Actor binding
rules.

---

## Manual peer connections (configuration time·runtime)

Connects manually to a specific endpoint without automatic discovery. Call it through
`MeshNodeBuilder.PeerConnections`.

```csharp
play.PeerConnections.Connect(RoutingId.From("play-node-2"), "play-node-2:5501");
IReadOnlyList<ZLinkMeshPeerConnection> connections = play.PeerConnections.ListConnections();
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Connect(endpoint)` | no expected RID | the admission handshake determines the remote identity |
| `.Connect(expectedRoutingId, endpoint)` | — | does not admit the connection if the handshake identity differs |
| `.Disconnect(endpoint)` | — | releases a registered connection |
| `.ListConnections()` | — | lists the currently registered connections |

**Completion.** Registers/releases synchronously with no return value. If both MeshNodes are
Object Clients and neither has RouteMesh Channel Server membership, this connection intent can
remain in the list without becoming a ready peer, and it is excluded from the ready-peer count
and the liveness target set. If either side has Channel Server membership that includes weight
`0`, ordinary peer admission·liveness rules apply.

**When to use it.** Use it to configure RouteMesh with a fixed peer list instead of automatic
discovery (Location Store).

---

## `UseFilter<TFilter>` (configuration time)

Inserts common logic (authentication, logging, etc.) in front of every handler dispatch.

```csharp
options.UseFilter<AuthenticationFilter>();

public sealed class AuthenticationFilter : IZLinkHandlerFilter
{
    public async ValueTask InvokeAsync(
        IZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext next,
        CancellationToken cancellationToken)
    {
        if (!IsAuthenticated(context))
        {
            return; // not calling next() ends the request as Rejected
        }

        await next();
    }
}
```

**Options.** The following modifier attaches to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.UseFilter<TFilter>()` | none (runs in registration order) | adds an `IZLinkHandlerFilter` implementation to the dispatch chain |

**Completion.** Registers synchronously with no return value. Calling `next()` runs the
remaining filters and the handler. Not calling `next()` on a request ends it with a `Rejected`
reply; calling `next()` twice fails with `InvalidOperation` and does not run the handler again.
`context.DispatchKind` distinguishes
`NodeDirectSend`/`NodeDirectRequest`/`ChannelSend`/`ChannelRequest`/`ClassicFanout` —
`ChannelSend`/`ChannelRequest` cover both RouteMesh and ClientServer.

**When to use it.** Use it when common preprocessing or validation needs to repeat for every
handler. A filter never builds the business reply directly — it only expresses rejection, and
the handler handles the rest.

---

## Other host-wide options (configuration time)

Simple property-bag configuration `IZLinkFrameworkOptions` provides. Each setting is called
independently.

```csharp
services.AddZLinkFramework(options =>
{
    options.AddHandlersFromAssemblyOf<GameEntrySpot>(); // finds and registers attribute-marked handlers from an assembly
    options.ConfigureNetwork().BindHost = "0.0.0.0";
    options.ConfigureInboundDispatch().ApplicationHwmProfile = ZLinkApplicationHwmProfile.LowLatency;
    options.ConfigureMetadata()
        .AllowSessionToActor("trace-id")
        .AllowActorToSession("server-region"); // the metadata-key allowlist to forward
    options.ConfigureStreamCompression().UseLz4();
});
```

**Options.** Commonly used items:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.AddHandlersFromAssemblyOf<TMarker>()` / `.AddHandlersFromAssembly(assembly)` / `.AddHandlersFromAssemblyOf(Type markerType)` | implicit auto-registration is active | specifies the target assembly for attribute-based handler scanning. All three overloads perform the same scan |
| `.DisableImplicitHandlerAutoRegistration()` | active (auto scan) | turns off attribute-based auto-registration and uses only explicit builder registration |
| `.ConfigureMetadata().AllowSessionToActor(key)` / `.AllowActorToSession(key)` | a key not specified is not forwarded | adds a metadata key to the per-direction allowlist to forward over STREAM session↔Actor relay |
| `.ConfigureNetwork()` | `BindHost` is all interfaces | the default bind·advertise host used unless an individual Listen call overrides it. Returns `IZLinkNetworkOptions` (a property bag) |
| `.ConfigureInboundDispatch()` | `ApplicationHwmProfile = Balanced` | the inbound application HWM size·profile, and the process memory cap. Returns `IZLinkInboundDispatchOptions` |
| `.ConfigureDispatch().Unhandled` | Framework default policy | how a packet with no matching handler is handled |
| `.ConfigureStreamCompression()` | no compression | the default STREAM compression codec (`UseDefault()`/`UseLz4()`/`Use(codec)`/`Disable()`) |
| `.ConfigureRouterSocket()` / `.ConfigureSpotPublisher()` (on `MeshNodeBuilder`) | socket defaults | individual HWM·buffer·timeout tuning for the MeshNode ROUTER socket and the Spot publisher |

**Completion.** `.AddHandlersFromAssemblyOf(...)`/`.DisableImplicitHandlerAutoRegistration()`
run synchronously with no return value.
`.ConfigureMetadata()`/`.ConfigureNetwork()`/`.ConfigureInboundDispatch()`/`.ConfigureDispatch()`/`.ConfigureStreamCompression()`
synchronously return the corresponding builder or options object, on which properties are set or
further modifiers are called. A value outside range surfaces as `ZLinkConfigurationException`
during host startup validation.

**When to use it.** Use it to adjust host-wide settings that end in a single simple value and
do not belong to one of the dedicated entries above (host lifecycle·topology registration·
diagnostics). For diagnostics-related settings, use the observability-diagnostics category.

---

## Runtime weight read·write

Changes the placement weight or channel weight without redeploying.

```csharp
IZLinkMeshPlacementRuntimeOptions placement = routeMeshRuntimeOptions.Mesh("play");
placement.PlacementWeight = 50; // lowers this node's share of new Actor·Spot placement

IZLinkMeshChannelRuntimeOptions channel = routeMeshRuntimeOptions.Channel("play.api");
channel.Weight = 0; // excludes this Channel Server from selection
```

**Options.** This entry point has two independent properties.

| Property | Default | Meaning |
| --- | --- | --- |
| `Mesh(meshName).PlacementWeight` | the value set at registration | node-scoped Actor·Spot placement weight |
| `Channel(channelName).Weight` | the value set at registration | ChannelName-scoped Server selection weight |

**Completion.** A synchronous get/set. It applies immediately, with no separate completion
signal.

**When to use it.** Use it to adjust placement or traffic share while operating. Transport
options including `MaxMessageSize` cannot change through this path — they are set only before
startup.

---

## Topology status read·observe

Checks the operational status of RouteMesh·ClientServer·Fanout individually. The three
runtimes share the same shape — one `GetStatus` read, or streamed observation via
`ObserveAsync`.

```csharp
ZLinkRouteMeshStatus status = routeMeshRuntime.GetStatus("play");
bool canPlaceNewObjects = status.IsReady && status.Placement.IsAvailable;

await foreach (var observed in routeMeshRuntime.ObserveAsync("play", ct))
{
    // check observed.Status.Channels, observed.Status.Peers
}
```

**Options.** The three runtimes correspond as follows.

| Runtime | Target | Returned status |
| --- | --- | --- |
| `IZLinkRouteMeshRuntime` | MeshName | `ZLinkRouteMeshStatus` (includes Channels, Peers, Placement) |
| `IZLinkClientServerRuntime` | ChannelName | `ZLinkClientServerStatus` (includes Targets) |
| `IZLinkFanoutRuntime` | ChannelName | `ZLinkFanoutStatus` (includes Publishers) |

**Completion.** `GetStatus` is a synchronous call that returns a value immediately.
`ObserveAsync` streams `ZLinkObservedStatus<TStatus>` in the same shape as the host-lifecycle
category's `ObserveAsync`, and the `Loss` field tells whether an observation was lost.
Querying a manual ChannelName through `IZLinkFanoutRuntime` completes with
`ZLinkConfigurationException`.

**When to use it.** Use it to judge the availability of a specific MeshName·ChannelName or
narrow the scope of a failure. If the entire host's status is needed, use the host-lifecycle
category's `Status`/`ObserveAsync`.

---

The full basis is
[RouteMesh·MeshNode exact interface](../../common/spec/server/languages/dotnet/interfaces/03-configuration-topology.en.md) and
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.en.md)
(both Korean-only).
