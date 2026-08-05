# 02. Topology discovery

[Reference index](README.en.md)

This category covers the topology registration entry points `zlinkFramework()`
(`ZLinkNestFrameworkOptionsBuilder`) provides, and the entry points that query
RouteMesh/ClientServer/Fanout operational status. The exact signatures are owned by the
[Foundation types and configuration exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.en.md),
the
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.en.md),
and the
[Location operational query and observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
(Korean-only). Every registration entry point is a configuration-time call inside the
`zlinkFramework()` chain.

---

## `addRouteMesh` (configuration time)

Registers one physical MeshNode. The starting point for RouteMesh-based topology.

```ts
const play = zlinkFramework()
  .addRouteMesh("play")
  .listen(5501)
  .setRoutingIdPrefix("play")
  .setPlacementWeight(100);
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.listen(endpoint)` / `.listen(port?)` | Does not bind if omitted | The receiving endpoint for this MeshNode |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` | Follows `configureNetwork()`'s root default (BindHost `127.0.0.1`) | The bind/advertise host that applies only to this MeshNode |
| `.routingId(routingId)` / `.setRoutingIdPrefix(prefix)` | Issued by the Framework | A fixed RID, or the prefix of an issued RID |
| `.setPlacementWeight(weight)` | 100 (range `0..10000`) | The relative weight for placing new Actors/Spots on this node |
| `.setActorLimit(limit)` / `.setSpotLimit(limit)` | Framework profile default | The Actor/Spot capacity this node accepts |
| `.setActivationConcurrency(limit)` | Framework default | The concurrent-execution cap for activation admission |
| `.setDefaultRequestTimeout(timeoutMs)` (raw builder) | This MeshNode's default request timeout | The value `requestToNode`/`requestToChannel` (messaging-execution category) uses when `.timeout(...)` is omitted |
| `.setInstanceSpotIdleTimeout(timeoutMs)` | `0` (never reclaims) | The Instance Spot idle reclaim time (ms) |
| `.configureRouterSocket()` | `ZLinkMeshNodeSocketConfig` default | This MeshNode's ROUTER socket HWM/buffer/timeout (`maxMessageSize` defaults to `16_777_216`, etc.) |
| `.configureSpotPublisher()` | `ZLinkSpotPublisherConfig` default | The Logical Multicast publisher socket's HWM/timeout/linger |
| `.objects()` | — | Enters Object role registration. See the Object role registration entry |
| `.channel(channelName)` | — | Enters this MeshNode's RouteMesh Channel role registration. See the RouteMesh Channel registration entry |
| `.peerConnections()` | — | See the Manual peer connections entry |
| `.addSendHandler(packetName, handlerType)` / `.addRequestHandler(packetName, handlerType)` (NestJS builder) | — | Registers a Node-direct handler. The target `sendToNode`/`requestToNode` (messaging-execution category) calls |

**Completion result.** Registers synchronously with no return value. An invalid combination (a
duplicate MeshName, a missing listener setting, etc.) surfaces as a `ZLinkConfigurationException`
in startup validation when `ZLinkModule.forRoot(...)` initializes.

**When to use.** Every host that uses RouteMesh registers at least one MeshNode. A node that only
uses manual peers and needs no distributed discovery can start without a Location Store.

---

## Object role registration (configuration time)

Registers how a MeshNode treats Actors/Spots (whether it only acts as a Client, or hosts them as a
Server).

```ts
play.objects().server()
  .addEntrySpot(GameEntrySpot)
  .addSpotFactory("room", RoomSpot, (factory) => {
    factory.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    factory.preserveStateWith(RoomRelocationAdapter);
  })
  .addActorFactory("player", PlayerActorFactory, (factory) => {
    factory.preserveStateWith(PlayerRelocationAdapter);
  });
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.client()` / `.server()` | `None` if omitted | Selects the Object Client (reference only) or Object Server (hosting) role. `server()` includes `client()` capability, and both require a Location Store |
| `.addEntrySpot(entrySpotType)` | None | Registers an Entry Spot type dedicated to external entry |
| `.addSpotFactory(spotType, implementation, configure)` | None | Registers a stable User Spot type. `configure` must call exactly one of `disableRelocation()`/`recreateOnRelocation()`/`preserveStateWith(...)`, in addition to `stableTypeLimit`/`executionMode`/`relocationReadiness` |
| `.addInstanceSpotFactory(instanceSpotType, implementation, configure)` | None | Registers a cold-activation Instance Spot type. `configure` must call exactly one relocation behavior, in addition to `stableTypeLimit` |
| `.addActorFactory(actorType, factoryType, configure)` | None | Registers a stable Actor type. `configure` must call exactly one relocation behavior (an Actor factory has no `stableTypeLimit`) |

**Completion result.** Registers synchronously with no return value. Omitting the relocation
behavior, or calling more than one, is a startup configuration error.

**When to use.** Register the corresponding role when this node actually hosts Actors/Spots
(Server), or only references Actors/Spots another node hosts as a messaging target (Client). See
the actor-relocation category for relocation-policy selection criteria.

---

## RouteMesh Channel registration (configuration time)

Registers logical ChannelName membership within the same MeshNode.

```ts
play.channel("play.api").server()
  .setWeight(100)
  .addHandlerGroup("api");

play.channel("play.events").client();
```

**Options.** After `channel(channelName)`, call `.client()` or `.server()` exactly once.
`.client()` only creates the send path and has no modifiers (the TypeScript type layer prevents
an invalid role setting). Commonly used modifiers of `.server()` are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.setWeight(weight)` | 100 (range `0..10000`) | The relative weight for this Server to be selected as a request/send target. `0` excludes it from selection |
| `.addHandlerGroup(groupName)` | None | Specifies a handler group marked with the `@ZlinkHandlerGroup(groupName)` decorator |
| `.addSendHandler(handlerType)` / `.addRequestHandler(handlerType)` (raw builder) | The packet name is determined from the handler decorator | Registers a one-way/request-reply handler directly on this channel |
| `.addSendHandler(packetName, handlerType)` / `.addRequestHandler(packetName, handlerType)` (NestJS builder) | — | The equivalent registration on the NestJS surface that specifies the packet name explicitly |

**Completion result.** Registers synchronously with no return value. A duplicate handler key
under the same owner surfaces as a `ZLinkConfigurationException` in startup validation.

**When to use.** Use `.server()` when registering a handler that `sendToChannel`/
`requestToChannel` (messaging-execution category) will receive. If this MeshNode only calls
another node's Server and places no handler of its own, register only `.client()`. Use
`addClientServerChannel` instead if communication must cross different processes.

---

## `addClientServerChannel` (configuration time)

Registers an independent ClientServer Channel unrelated to RouteMesh.

```ts
zlinkFramework().addClientServerChannel("payments.api").server()
  .listen(6001)
  .setWeight(100)
  .addRequestHandler("charge", ChargeHandler);

zlinkFramework().addClientServerChannel("payments.api").client()
  .connect("payments-1:6001");
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.server().listen(port?)` | Automatic bind | This Server's receiving port |
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

```ts
zlinkFramework().addFanoutChannel("lobby.events")
  .enablePublisher(7001)
  .addHandlerGroup("events");

// automatic subscriber — automatically discovers publishers of the same ChannelName from the
// location store.
zlinkFramework().addFanoutChannel("lobby.events").enableSubscriber();

// manual subscriber — uses only the specified endpoint.
zlinkFramework().addFanoutChannel("lobby.events").enableSubscriber("lobby-1:7001");
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.enablePublisher(endpoint)` / `.enablePublisher(port?)` | None | Registers this channel's publisher role and receiving endpoint |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` / `.routingId(rid)` / `.setRoutingIdPrefix(prefix)` | Root default, or issued by the Framework | The bind/advertise host and RID that apply only to the publisher |
| `.enableSubscriber()` (raw builder, no endpoint) | — | automatic subscriber. Finds every valid publisher of the same ChannelName from the Location Store |
| `.enableSubscriber(endpoint)` (NestJS builder) / `.connect(endpoint)` (raw builder) | — | manual subscriber. Uses only the specified endpoint |
| `.subscriberConnections()` (raw builder) | — | Returns a runtime handle (`ZLinkEndpointConnections`: `connect`/`disconnect`/`listConnections`) over the set of manual subscriber endpoints |
| `.getListenerStatus(channelName)` (runtime, `ZLinkFanoutClient`) | — | Queries the current advertised endpoint after the publisher listener has bound |

**Completion result.** Registers synchronously with no return value. Configuring both automatic
subscriber and manual subscriber on the same fanout channel surfaces as a startup failure.
`getListenerStatus(...)` fails with `ZLinkConfigurationException` if the host has not started, or
that channel is not registered as a publisher.

**When to use.** Use this when creating a new observation/notification channel where the
publisher need not know its subscribers. If a reply is needed, use RouteMesh Channel or
ClientServer Channel registration instead.

---

## `addStreamNode` (configuration time)

Registers a listener that accepts external STREAM connections.

```ts
zlinkFramework().addStreamNode("public-gateway")
  .bind(9001)
  .enableActorDispatch()
  .registerSession(GameSession);
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.bind(endpoint)` / `.bind(port?)` | Automatic bind | This STREAM listener's receiving port |
| `.setBindHost(host)` / `.setAdvertiseHost(host)` | Root default | The bind/advertise host that applies only to this listener |
| `.setTlsServer(certPath, keyPath, requireClientCertificate?)` | No TLS | TLS server certificate/key, and whether to require mutual authentication |
| `.enableActorDispatch()` | Disabled | Dispatches an incoming message to a bound Actor via global ActorId lookup |
| `.registerSession(sessionType)` | None | Registers a Session type implementing `ZLinkSession` (or a `ZLinkSessionFactory`) |

**Completion result.** Registers synchronously with no return value. A TLS configuration error
surfaces as a `ZLinkConfigurationException` in startup validation.

**When to use.** Use this to open a gateway that external clients connect to directly over the
STREAM protocol. See the stream-session category for the exact Session/Actor wiring rules.

---

## Manual peer connections (configuration time and runtime)

Connects to a specific endpoint manually, without automatic discovery. Called via
`ZLinkMeshNodeBuilder.peerConnections()`.

```ts
play.peerConnections().connect("play-node-2:5501");
const connections = play.peerConnections().listConnections();
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
membership, including one with weight `0`, it makes the connection and keeps liveness.

**When to use.** Use this to configure RouteMesh with a fixed peer list, without automatic
discovery (a Location Store).

---

## `filters` / `ZLinkHandlerFilter` (configuration time)

Inserts common logic (authentication, logging, etc.) in front of every handler dispatch.
Registered via the `filters` array of `zlinkFramework().options({...})`.

```ts
zlinkFramework().options({
  filters: [AuthenticationFilter],
});

class AuthenticationFilter implements ZLinkHandlerFilter {
  async invoke(context: ZLinkHandlerFilterContext, next: ZLinkHandlerFilterNext) {
    if (!isAuthenticated(context)) {
      return; // not calling next() ends the request as Rejected
    }
    await next();
  }
}
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `options({ filters })` | None (runs in registration order) | Adds an array of `ZLinkHandlerFilter` implementation types to the dispatch chain |

**Completion result.** Registers synchronously with no return value. Calling `next()` runs the
remaining filters and the handler. Not calling `next()` in a request ends it as `Rejected`, and
calling `next()` twice fails with `InvalidOperation` without re-running the handler.
`context.dispatchKind` distinguishes `NodeDirectSend`/`NodeDirectRequest`/`ChannelSend`/
`ChannelRequest`/`ClassicFanout` — `ChannelSend`/`ChannelRequest` include both RouteMesh and
ClientServer.

**When to use.** Use this when common preprocessing/validation must repeat across individual
handlers. A filter does not construct the business reply itself — it only expresses rejection,
and the handler does the rest. Does not apply to Spot/Actor/Logical Multicast/STREAM handlers.

---

## Other host-wide options (configuration time)

Configuration that ends with a single simple value, which `ZLinkNestFrameworkOptionsBuilder`
provides.

```ts
zlinkFramework()
  .configureNetwork()
  .bindHost = "0.0.0.0";

zlinkFramework()
  .configureInboundDispatch()
  .applicationHwmProfile(ZLinkApplicationHwmProfile.LowLatency);

zlinkFramework().configureStreamCompression().useLz4();
zlinkFramework().setApplicationVersion(2n);
zlinkFramework().options({ requestTimeoutMs: 30_000, worker: { minThreads: 2, maxThreads: 8 } });
```

**Options.** Commonly used entries are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.configureNetwork()` | `bindHost` is `127.0.0.1` | The default bind/advertise host used unless an individual listen call overrides it |
| `.configureInboundDispatch()` | `ZLinkApplicationHwmProfile.Balanced` | The inbound application HWM size/profile, and the process memory cap |
| `.configureDispatch()` | Framework default policy | Dispatch/diagnostics options. See the observability-diagnostics category |
| `.configureStreamCompression()` | No compression | The STREAM default compression codec (`useDefault()`/`useLz4()`/`use(codec)`/`disable()`) |
| `.setApplicationVersion(version)` / `.setMaintenanceWave(waveId)` | `0n` / none (no exclusion) | The deployment version and maintenance wave every local MeshNode publishes |
| `.options({ requestTimeoutMs, worker, dispatch, metrics, filters })` | Each field's default | Specifies the host-wide request timeout, worker pool, dispatch options, metrics integration, and filters all at once |
| `.codecs()` | Only JSON registered | `zlinkFramework().codecs().use(extension)`. See the Codec registration entry in messaging-execution category |

**Completion result.** Most execute synchronously with no return value;
`.configureNetwork()`/`.configureInboundDispatch()`/`.configureDispatch()`/`.codecs()` return the
corresponding builder or options object to continue further configuration on. Exceeding a value's
range surfaces as a configuration error in startup validation.

**When to use.** Use this to adjust host-wide settings that end with a single simple value and do
not belong to a dedicated category above (host lifecycle, topology registration, diagnostics).

---

## Runtime weight query/change

Changes placement weight or channel weight without redeploying. Provided by
`ZLinkRouteMeshRuntimeOptions`, injected via the `ZLINK_CHANNEL_RUNTIME_OPTIONS` DI token.

```ts
routeMeshRuntimeOptions.mesh("play").placementWeight = 50;
routeMeshRuntimeOptions.channel("play.api").weight = 0;
```

**Options.** This entry point has two independent properties.

| Property | Default | Meaning |
| --- | --- | --- |
| `mesh(meshName).placementWeight` | The value at registration time | The node-level Actor/Spot placement weight |
| `channel(channelName).weight` | The value at registration time | The ChannelName-level Server selection weight |

**Completion result.** A synchronous get/set. It applies immediately with no separate completion
signal.

**When to use.** Use this to adjust placement or traffic share while running. Transport options,
including `maxMessageSize`, cannot be changed through this path — configure them only before
startup.

---

## Topology status query/observation

Checks the operational status of each of RouteMesh/ClientServer/Fanout. The three runtimes
(`ZLINK_ROUTE_MESH_RUNTIME`/`ZLINK_CLIENT_SERVER_RUNTIME`/`ZLINK_FANOUT_RUNTIME` DI tokens)
provide the same shape (one `snapshot` query, streaming observation with `observe`).

```ts
const status = routeMeshRuntime.snapshot("play");
const canPlaceNewObjects = status.isReady && status.placement.isAvailable;

for await (const observed of routeMeshRuntime.observe("play", /*capacity=*/64)) {
  // check observed.status.channels, observed.status.peers
}
```

**Options.** The correspondence among the three runtimes is as follows.

| Runtime | Target | Returned status |
| --- | --- | --- |
| `ZLinkRouteMeshRuntime` | MeshName | `ZLinkRouteMeshStatus` (includes channels, peers, placement) |
| `ZLinkClientServerRuntime` | ChannelName | `ZLinkClientServerStatus` (includes targets) |
| `ZLinkFanoutRuntime` | ChannelName | `ZLinkFanoutStatus` (includes publishers) |

**Completion result.** `snapshot(...)` is a synchronous call that returns a value immediately.
`observe(...)` returns `AsyncIterable<ZLinkObservedStatus<TStatus>>`, and the `loss` field tells
you whether observations were lost. Querying an unregistered name fails with a typed route error
instead of creating a new status.

**When to use.** Use this to judge a specific MeshName/ChannelName's availability, or to narrow
the scope of a failure. If host-wide status is needed, use `status`/`observe` in the
host-lifecycle category.

---

See the
[Foundation types and configuration exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.en.md),
the
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.en.md),
and the
[Location operational query and observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
(Korean-only) for the full rationale.
