# 02. Topology discovery

[Reference index](README.en.md)

This category covers the topology registration entry points `ZLinkFrameworkOptions` provides,
and the entry points that query RouteMesh/ClientServer/Fanout operational status. The exact
signatures are owned by the
[Java configuration and host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.en.md)
and the
[Java channel messaging exact interface](../../common/spec/server/languages/java/interfaces/channel-messaging.en.md)
(Korean-only). Every registration entry point is a configuration-time call made inside
`ZLinkFrameworkConfigurer.configure(...)`.

---

## `addRouteMesh` (configuration time)

Registers one physical MeshNode. The starting point for RouteMesh-based topology.

```java
ZLinkMeshNodeBuilder play = options.addRouteMesh("play")
    .listen(5501)
    .setRoutingIdPrefix("play")
    .setPlacementWeight(100);
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.listen(endpoint)` / `.listen()` / `.listen(port)` | Does not bind if omitted | The receiving endpoint for this MeshNode |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` | Follows `configureNetwork()`'s root default (BindHost `127.0.0.1`) | The bind/advertise host that applies only to this MeshNode |
| `.setRoutingId(routingId)` / `.setRoutingIdPrefix(prefix)` | Issued by the Framework | A fixed RID, or the prefix of an issued RID (1..64 ASCII characters of `[A-Za-z0-9._-]`) |
| `.setPlacementWeight(weight)` | 100 (range `0..10000`) | The relative weight for placing new Actors/Spots on this node |
| `.setActorCapacity(max)` / `.setSpotCapacity(max)` | active 10,000 / pending 128 | The Actor/Spot capacity this node accepts |
| `.setActivationConcurrency(max)` | Framework default | The concurrent-execution cap for activation admission |
| `.setDefaultRequestTimeout(timeout)` | This MeshNode's default request timeout | The value `requestToNode`/`requestToChannel` (messaging-execution category) uses when `.timeout(...)` is omitted |
| `.setInstanceSpotIdleTimeout(timeout)` | `Duration.ZERO` (never reclaims) | The Instance Spot idle reclaim time |
| `.configureRouterSocket()` | `ZLinkMeshNodeSocketConfig` default | This MeshNode's ROUTER socket HWM/buffer/timeout (`maxMessageSize` defaults to `16_777_216L`, etc.) |
| `.configureSpotPublisher()` | `ZLinkSpotPublisherConfig` default | The Logical Multicast publisher socket's HWM/timeout/linger |
| `.objects()` | — | Enters Object role registration. See the Object role registration entry |
| `.channel(channelName)` | — | Enters this MeshNode's RouteMesh Channel role registration. See the RouteMesh Channel registration entry |
| `.peerConnections()` | — | See the Manual peer connections entry |
| `.addRouteSendHandler(handlerType, messageType)` | The packet name is determined from the message type | Registers a Node-direct one-way handler. The target `sendToNode` (messaging-execution category) calls |
| `.addRouteRequestHandler(handlerType, requestType, replyType)` | The packet name is determined from the message type | Registers a Node-direct request handler. The target `requestToNode` calls |

**Completion result.** Registers synchronously with no return value. An invalid combination (a
duplicate MeshName, a missing listener setting, etc.) surfaces as a `ZLinkConfigurationException`
in startup validation at Spring context initialization time.

**When to use.** Every host that uses RouteMesh registers at least one MeshNode. A node that only
uses manual peers and needs no distributed discovery can start without a Location Store.

---

## Object role registration (configuration time)

Registers how a MeshNode treats Actors/Spots (whether it only acts as a Client, or hosts them as a
Server).

```java
play.objects().server()
    .addEntrySpot(GameEntrySpot.class)
    .addSpotFactory("room", RoomSpot.class, factory -> factory
        .executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
        .preserveStateWith(RoomRelocationAdapter.class))
    .addActorFactory("player", PlayerActor.class, PlayerActorFactory.class, factory ->
        factory.preserveStateWith(PlayerRelocationAdapter.class));
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.client()` / `.server()` | — | Selects the Object Client (reference only) or Object Server (hosting) role. `server()` includes `client()` capability, and both require a Location Store. `None` if omitted |
| `.addEntrySpot(entrySpotClass)` | None | Registers an Entry Spot type dedicated to external entry |
| `.addSpotFactory(spotType, spotClass, configure)` | None | Registers a stable User Spot type. `configure` must call exactly one of `disableRelocation()`/`recreateOnRelocation()`/`preserveStateWith(...)`, in addition to `stableTypeLimit`/`executionMode`/`relocationReadiness` |
| `.addInstanceSpotFactory(instanceSpotType, spotClass, configure)` | None | Registers a cold-activation Instance Spot type. `configure` must call exactly one relocation behavior, in addition to `stableTypeLimit` |
| `.addActorFactory(actorType, actorClass, factoryClass, configure)` | None | Registers a stable Actor type. `configure` must call exactly one relocation behavior (an Actor factory has no `stableTypeLimit`) |

**Completion result.** Registers synchronously with no return value. The Framework runs the
`configure` callback synchronously exactly once inside the registration call — calling a retained
builder again after the callback returns is a configuration error. Omitting the relocation
behavior, or calling more than one, is a startup configuration error.

**When to use.** Register the corresponding role when this node actually hosts Actors/Spots
(Server), or only references Actors/Spots another node hosts as a messaging target (Client). See
the actor-relocation category for relocation-policy selection criteria.

---

## RouteMesh Channel registration (configuration time)

Registers logical ChannelName membership within the same MeshNode.

```java
play.channel("play.api").server()
    .setWeight(100)
    .addHandlerGroup("api")
    .addRequestHandler(GetPlayerHandler.class, GetPlayer.class, Player.class);

play.channel("play.events").client();
```

**Options.** After `channel(channelName)`, call `.client()` or `.server()` exactly once.
`.client()` only creates the send path and has no modifiers. Commonly used modifiers of
`.server()` are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.setWeight(weight)` | 100 (range `0..10000`) | The relative weight for this Server to be selected as a request/send target. `0` excludes it from selection |
| `.addHandlerGroup(groupName)` | None | Specifies the handler group that annotation-based handler scanning finds |
| `.addSendHandler(handlerType, messageType)` | The packet name is determined from the message type | Registers a one-way handler directly on this channel |
| `.addRequestHandler(handlerType, requestType, replyType)` | The packet name is determined from the message type | Registers a request/reply handler directly on this channel |

**Completion result.** Registers synchronously with no return value. A duplicate handler key
under the same owner surfaces as a `ZLinkConfigurationException` in startup validation.

**When to use.** Use `.server()` when registering a handler that `sendToChannel`/
`requestToChannel` (messaging-execution category) will receive. If this MeshNode only calls
another node's Server and places no handler of its own, register only `.client()`. Use
`addClientServerChannel` instead if communication must cross different processes.

---

## `addClientServerChannel` (configuration time)

Registers an independent ClientServer Channel unrelated to RouteMesh.

```java
options.addClientServerChannel("payments.api").server()
    .listen(6001)
    .setWeight(100)
    .addRequestHandler(ChargeHandler.class, Charge.class, ChargeResult.class);

options.addClientServerChannel("payments.api").client()
    .connect("payments-1:6001");
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.server().listen()` / `.listen(port)` | Automatic bind | This Server's receiving port |
| `.server().setBindHost(host)` / `.setAdvertiseHost(host)` | Root default | The bind/advertise host that applies only to this Server |
| `.server().setWeight(weight)` / `.addSendHandler`/`.addRequestHandler` | Same as RouteMesh Channel Server | Weight and handler registration |
| `.client().connect(endpoint)` | manual | Connects to a specific Server manually. Omitting it finds the target via automatic discovery |

**Completion result.** Registers synchronously with no return value. A Client/Server using
automatic discovery without a Location Store registration surfaces as a configuration error in
startup validation. Client and Server can each be registered once on the same ChannelName, but
registering the same role twice fails startup.

**When to use.** Use this for request/reply or one-way messaging between independent services
that are not RouteMesh members. Between nodes in the same RouteMesh, use RouteMesh Channel
registration instead.

---

## `addFanoutChannel` (configuration time)

Registers a channel dedicated to classic fanout. It is the target `ZLinkFanoutClient.publish`
(messaging-execution category) publishes to.

```java
options.addFanoutChannel("lobby.events")
    .enablePublisher(7001)
    .addHandlerGroup("events");

// automatic subscriber — automatically discovers publishers of the same ChannelName from the
// location store.
options.addFanoutChannel("lobby.events")
    .enableSubscriber();

// manual subscriber — uses only the specified endpoints. Combining it with enableSubscriber()
// fails startup.
options.addFanoutChannel("lobby.events")
    .connect("lobby-1:7001");
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.enablePublisher(endpoint)` / `.enablePublisher()` / `.enablePublisher(port)` | None | Registers this channel's publisher role and receiving endpoint |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` / `.setRoutingId(rid)` / `.setRoutingIdPrefix(prefix)` | Root default, or issued by the Framework | The bind/advertise host and RID that apply only to the publisher |
| `.enableSubscriber()` | — | automatic subscriber. Finds every valid publisher of the same ChannelName from the Location Store |
| `.connect(endpoint)` | — | manual subscriber. Uses only the specified endpoint |
| `.subscriberConnections()` | — | Returns a runtime handle (`ZLinkEndpointConnections`: `connect`/`disconnect`/`listConnections`) over the set of manual subscriber endpoints |
| `.addHandlerGroup(groupName)` | None | Links a typed event handler group |

**Completion result.** Registers synchronously with no return value. Configuring both automatic
subscriber and manual subscriber on the same fanout channel surfaces as a startup failure.

**When to use.** Use this when creating a new observation/notification channel where the
publisher need not know its subscribers. If a reply is needed, use RouteMesh Channel or
ClientServer Channel registration instead.

---

## `addStreamNode` (configuration time)

Registers a listener that accepts external STREAM connections.

```java
options.addStreamNode("public-gateway")
    .bind(9001)
    .enableActorDispatch()
    .registerSession(GameSession.class);
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.bind(endpoint)` / `.bind()` / `.bind(port)` | Automatic bind | This STREAM listener's receiving port |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` | Root default | The bind/advertise host that applies only to this listener |
| `.configureSocket()` | `ZLinkStreamSocketConfig` default | Fine-tunes this listener socket's `maxMessageSize` and more |
| `.setTlsServer(certPath, keyPath)` / `.setTlsServer(certPath, keyPath, requireClientCertificate)` | No TLS | TLS server certificate/key, and whether to require mutual authentication |
| `.enableActorDispatch()` | Disabled | Dispatches an incoming message to a bound Actor via global ActorId lookup |
| `.registerSession(sessionClass)` | None | Registers a Session type implementing `ZLinkSession` |
| `.addSessionPacketHandler(handlerType)` | None | Adds a typed packet handler for the Session to process |

**Completion result.** Registers synchronously with no return value. A TLS configuration error
surfaces as a `ZLinkConfigurationException` in startup validation.

**When to use.** Use this to open a gateway that external clients connect to directly over the
STREAM protocol. See the stream-session category for the exact Session/Actor wiring rules.

---

## Manual peer connections (configuration time and runtime)

Connects to a specific endpoint manually, without automatic discovery. Called via
`ZLinkMeshNodeBuilder.peerConnections()`.

```java
play.peerConnections().connect("play-node-2:5501");
List<ZLinkMeshPeerConnection> connections = play.peerConnections().listConnections();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.connect(endpoint)` | No expected RID | The admission handshake determines the remote identity |
| `.connect(expectedRoutingId, endpoint)` | — | Does not admit if the handshake identity differs |
| `.disconnect(endpoint)` | — | Releases a registered connection |
| `.listConnections()` | — | Queries the currently registered connection list |

**Completion result.** Registers/releases synchronously with no return value. If both MeshNodes
are Object Clients and neither has RouteMesh Channel Server membership, this connection intent
stays in the list but never becomes a ready peer. If either side has any Channel Server
membership, ordinary peer admission/liveness rules apply even if the weight is `0`.

**When to use.** Use this to configure RouteMesh with a fixed peer list, without automatic
discovery (a Location Store).

---

## `useFilter` (configuration time)

Inserts common logic (authentication, logging, etc.) in front of every handler dispatch.

```java
options.useFilter(AuthenticationFilter.class);

public class AuthenticationFilter implements ZLinkHandlerFilter {
    @Override
    public <T> CompletionStage<T> invoke(
        ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
        if (!isAuthenticated(context)) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("rejected"));
        }
        return next.invoke();
    }
}
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.useFilter(filterType)` | None (runs in registration order) | Adds a `ZLinkHandlerFilter` implementation to the dispatch chain |

**Completion result.** Registers synchronously with no return value. Calling `next.invoke()` runs
the remaining filters and the handler. Not calling `next` in a request ends it as `REJECTED`, and
calling it twice rejects with `IllegalStateException` without re-running the handler.
`context.dispatchKind()` distinguishes `NODE_DIRECT_SEND`/`NODE_DIRECT_REQUEST`/`CHANNEL_SEND`/
`CHANNEL_REQUEST`/`CLASSIC_FANOUT`.

**When to use.** Use this when common preprocessing/validation must repeat across individual
handlers. A filter does not construct the business reply itself — it only expresses rejection,
and the handler does the rest. Does not apply to Spot/Actor/Logical Multicast/STREAM handlers.

---

## Other host-wide options (configuration time)

Configuration that ends with a single simple value, which `ZLinkFrameworkOptions` provides.

```java
options.addHandlersFromPackageOf(GameEntrySpot.class); // finds and registers annotation-marked handlers from the package
options.configureNetwork().setBindHost("0.0.0.0");
options.configureInboundDispatch()
    .setApplicationHwmProfile(ZLinkApplicationHwmProfile.LOW_LATENCY);
options.configureMetadata()
    .allowSessionToActor("trace-id")
    .allowActorToSession("server-region");
options.configureStreamCompression().useLz4();
options.setApplicationVersion(2);
options.useVirtualThreadHandlers();
```

**Options.** Commonly used entries are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.addHandlersFromPackageOf(markerType)` | implicit auto-registration active | Specifies the target for annotation-based handler package scanning |
| `.configureMetadata().allowSessionToActor(key)` / `.allowActorToSession(key)` | Keys not specified are not forwarded | Adds a metadata key to forward across the STREAM session↔Actor relay to a direction-specific allowlist |
| `.configureNetwork()` | `bindHost()` is `127.0.0.1` | The default bind/advertise host used unless an individual listen call overrides it |
| `.configureWorkers()` | `ZLinkWorkerOptions` default | The bounded worker pool's minimum/maximum thread count, idle timeout, and queue cap |
| `.configureInboundDispatch()` | `ZLinkApplicationHwmProfile.BALANCED` | The inbound application HWM size/profile, and the process memory cap |
| `.configureDispatch()` | Framework default policy | Dispatch/diagnostics options. See the observability-diagnostics category |
| `.configureStreamCompression()` | No compression | The STREAM default compression codec (`useDefault()`/`useLz4()`/`use(codec)`/`disable()`) |
| `.setApplicationVersion(version)` / `.setMaintenanceWave(wave)` | `0` / `null` (no exclusion) | The deployment version and maintenance wave every local MeshNode publishes |
| `.setDefaultRequestTimeout(timeout)` | Framework default | The host-wide default request timeout |
| `.useVirtualThreadHandlers()` / `.useHandlerExecutor(executor)` | Implementation default executor | Selects the execution model for handler dispatch (virtual thread or a specified `Executor`). Mutually exclusive |
| `.codecs()` | Only JSON registered | `options.codecs().use(extension)`. See the Codec registration entry in messaging-execution category |

**Completion result.** Most execute synchronously with no return value; `.configureNetwork()`/
`.configureWorkers()`/`.configureInboundDispatch()`/`.configureDispatch()`/`.configureMetadata()`
return the corresponding builder or options object to continue further configuration on.
Exceeding a value's range surfaces as a configuration error in startup validation.

**When to use.** Use this to adjust host-wide settings that end with a single simple value and do
not belong to a dedicated category above (host lifecycle, topology registration, diagnostics).

---

## Topology status query/observation

Checks the operational status of each of RouteMesh/ClientServer/Fanout. The three runtimes provide
the same shape (one `snapshot` query, streaming observation with `observe`), and all are injected
as Spring beans.

```java
ZLinkMeshNodeSnapshot status = routeMeshRuntime.snapshot("play");
boolean canPlaceNewObjects = status.isReady() && status.placement().isAvailable();

routeMeshRuntime.observe("play", /*capacity=*/64)
    .subscribe(new Flow.Subscriber<>() { /* check observed.status() in onNext(observed) */ });
```

**Options.** The correspondence among the three runtimes is as follows.

| Runtime | Target | Returned snapshot |
| --- | --- | --- |
| `ZLinkRouteMeshRuntime` | MeshName | `ZLinkMeshNodeSnapshot` (includes channels, peers, placement) |
| `ZLinkClientServerRuntime` | ChannelName | `ZLinkClientServerStatus` (includes targets) |
| `ZLinkFanoutRuntime` | ChannelName | `ZLinkFanoutStatus` (includes publishers) |

**Completion result.** `snapshot(...)` is a synchronous call that returns a value immediately.
`observe(...)` returns `Flow.Publisher<ZLinkObservedStatus<TStatus>>`, and the `loss()` field
tells you whether observations were lost. Values only flow through `Flow.Publisher` after
`Subscription.request(n)` signals demand following `subscribe(...)`.

**When to use.** Use this to judge a specific MeshName/ChannelName's availability, or to narrow
the scope of a failure. If host-wide status is needed, use `status`/`observe` in the
host-lifecycle category.

---

See the
[Java configuration and host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.en.md)
and the
[Java channel messaging exact interface](../../common/spec/server/languages/java/interfaces/channel-messaging.en.md)
(Korean-only) for the full rationale.
