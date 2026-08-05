<!-- framework-adapter-nav:start -->
[Document list](../../../../../../README.en.md)
<!-- framework-adapter-nav:end -->

[Node spec table of contents](README.en.md)

# Node System Structure — Package, Registration, And Bootstrap

> This document owns **how ZLink framework is configured on top of
> NestJS**. Package structure, deployment, module bootstrap, DI,
> lifecycle, and each feature's **registration surface**.
>
> **The meaning and behavioral rules of a feature are owned by the
> common spec** — [channel-messaging](../../../08-channel-messaging.en.md),
> [spot-messaging](../../../12-spot-messaging.en.md), [MeshNode](../../../13-mesh-node.en.md),
> [stream-session](../../../19-stream-session.en.md), [actor-model](../../../14-actor-model.en.md),
> [session-actor-dispatch](../../../20-session-actor-dispatch.en.md),
> [runtime-monitoring](../../../24-runtime-monitoring.en.md),
> [location-runtime](../../../21-location-runtime.en.md),
> [channel-topology](../../../07-channel-topology.en.md).
>
> **Public types and signatures are owned per category by the
> [interface table of contents](interfaces/README.en.md).** This
> document only defines the Node framework's system structure and
> package boundary — it doesn't include usage examples or tutorials.
> The client connector is owned by
> [stream-connector](../../../stream-connector/languages/typescript/03-stream-connector.en.md).

## 1. Package Structure

| Package | Role | Dependencies |
|---|---|---|
| `@zlink-systems/framework` | Framework core — contract, runtime, dispatcher | `zlink`, `stream-wire`, OpenTelemetry API |
| `@zlink-systems/nestjs` | NestJS host adapter — the `ZLinkModule.forRoot(...)` registration surface | `framework`, NestJS common/core, `reflect-metadata`, `rxjs` |
| `@zlink-systems/framework-codec-protobuf` | Protobuf codec **extension** | `framework`, `stream-connector`, `protobufjs` |
| `@zlink-systems/framework-codec-msgpack` | MessagePack codec **extension** | `framework`, `stream-connector`, `@msgpack/msgpack` |
| `@zlink-systems/framework-locations-redis` | Redis location store **extension** | `framework`, `zlink`, `redis` |
| `@zlink-systems/http-client` | Fluent HTTP/JSON client | `framework`, `undici` |
| `@zlink-systems/stream-connector` | **Client** connector — doesn't depend on the server framework | `stream-wire` |
| `@zlink-systems/stream-wire` | The **wire layer** shared between connector and server | None |

**Separation principles:**

- **Codec implementations aren't mixed into core.** JSON is the default
  codec, and Protobuf/MessagePack are separated into extension
  packages. The current Node HTTP client doesn't use the framework
  codec registry. The common contract for codec sharing scope follows
  [Framework API §9](../../../06-framework-api.en.md#9-codec).
- **The [location store](../../../01-glossary.en.md#location-store)
  implementation is also an extension.** Core only knows the store
  contract — the Redis implementation is provided by a separate package
  (§10).
- **The connector doesn't reference the server framework package.** The
  reverse direction is the same.
- **`stream-wire` is environment-neutral.** It only uses `Uint8Array`
  and doesn't depend on `Buffer`, so it works with **the same code** in
  both Node and browsers.
- **The host adapter (`nestjs`) is separated from core.** Core doesn't
  depend on NestJS.

## 2. Deployment Contract

| Package | Deployment channel | Consumer |
|---|---|---|
| `@zlink-systems/framework` · `@zlink-systems/nestjs` | npm | Server application |
| `@zlink-systems/framework-codec-*` | npm | Server/browser client that needs a codec |
| `@zlink-systems/framework-locations-redis` | npm | Multi-process deployment |
| `@zlink-systems/stream-connector` | npm | Browser-family client |
| `@zlink-systems/stream-wire` | npm | Shared between connector and server |

**The TypeScript connector deploys one package root as ESM.** This
entrypoint uses the platform `WebSocket` in a browser-family client. A
separate browser subpath from the connector running in Node.js isn't
provided. The exact contract is owned by
[TypeScript Stream Connector](../../../stream-connector/languages/typescript/03-stream-connector.en.md).

## 3. Module Bootstrap

`ZLinkModule.forRoot(...)` / `forRootFactory(...)` is the registration
entrypoint.

**`forRoot(...)` is where transport/node/role/handler group selection is
declared — not where the application object graph is assembled.**

## 4. DI

- The outbound client and manager the framework exposes are registered
  as **NestJS provider tokens**. They're received with `@Inject(TOKEN)`,
  and the token is exported by the framework.
- **A handler receives dependencies through constructor injection, not
  as a service locator on the context. A DI container isn't put on the
  context.**
- An object the application implements **is owned by the NestJS DI
  container.** It isn't created directly with `new` in bootstrap code —
  it's registered in the module's `providers`.

| Object | Registration | When the framework resolves it |
|---|---|---|
| Channel/fanout/route handler | `providers` + handler registration surface | When the channel dispatches that handler group |
| Entry Spot, user Spot | `providers` + `addEntrySpot(...)` / `addSpotFactory(...)` | When the MeshNode/SpotManager activates a local Spot |
| Instance Spot | `providers` + `addInstanceSpotFactory(...)` | When a Spot direct fluent call starts Instance cold activation |
| [Spot](../../../01-glossary.en.md#spot) packet/subscribe/actor/timer handler | Handler decorator + `zlinkDiscoverProviders(...)` | When processed in that Spot's execution context |
| Actor factory | `providers` + `addActorFactory(...)` | When ActorManager creates an actor |
| Stream session (or [factory](../../../01-glossary.en.md#factory)) | `providers` + `streams` configuration | When a stream connection is activated as a session |

### 4.1 Provider Token

**The token symbol used for injection is exported by the
`@zlink-systems/nestjs` package root.**

**Always-registered providers:**

| Token | Surface |
|---|---|
| `ZLINK_CHANNEL_CLIENT` | Channel client |
| `ZLINK_ROUTE_CLIENT` | Route client |
| `ZLINK_FANOUT_CLIENT` | Fanout client |
| `ZLINK_BOUND_SESSION_FACTORY` | Bound session factory |
| `ZLINK_CHANNEL_RUNTIME_OPTIONS` | Channel runtime options |
| `ZLinkDrainHealthIndicator` | [MeshNode](../../../01-glossary.en.md#meshnode) readiness and health indicator |
| `ZLINK_MESSAGE_METADATA_POLICY` | Metadata policy |
| `ZLINK_FRAMEWORK_RUNTIME` · `ZLINK_FRAMEWORK_REGISTRATION` | Runtime and registration |

**Providers registered only when a role exists:**

| Token | Required role |
|---|---|
| `ZLINK_SPOT_MANAGER` · `ZLINK_SPOT_OUTBOUND` | Spot registration on the MeshNode |
| `ZLINK_SPOT_PUBLISHER_CLIENT` | Spot publisher role |
| `ZLINK_ACTOR_CLIENT` | Both MeshNode and location store are registered |
| `ZLINK_ACTOR_MANAGER` | The actor manager is active |
| `ZLINK_LOCATION_RUNTIME_QUERY` | At least one location store is registered |
| `ZLINK_ROUTE_MESH_RUNTIME` | At least one RouteMesh MeshNode is registered |
| `ZLINK_CLIENT_SERVER_RUNTIME` | At least one ClientServer Channel is registered |
| `ZLINK_FANOUT_RUNTIME` | At least one endpoint-less automatic fanout subscriber is registered |

**Injecting an unregistered token fails with NestJS's unresolved
dependency error.**

The `@zlink-systems/framework` package root exports the host-scoped
`ZLinkFrameworkRuntime`, `ZLinkRouteMeshRuntime`,
`ZLinkClientServerRuntime`, and `ZLinkFanoutRuntime` interfaces. The
`@zlink-systems/nestjs` package root exports the corresponding
`ZLINK_ROUTE_MESH_RUNTIME`, `ZLINK_CLIENT_SERVER_RUNTIME`,
`ZLINK_FANOUT_RUNTIME` tokens. NestJS's `ZLinkModule` registers, as the
provider for that token, a runtime instance satisfying the registration
condition in the table above, and exports the provider so it can be
injected outside the dynamic module too.

If there's no [RouteMesh](../../../01-glossary.en.md#routemesh) MeshNode
in a static `forRoot`, the RouteMesh runtime provider isn't created; if
there's no [ClientServer Channel](../../../01-glossary.en.md#clientserver-channel),
the ClientServer runtime provider isn't created. If there's only a
manual fanout subscriber, the fanout runtime provider isn't created.
When configuration is decided dynamically in `forRootFactory`, each
conditional provider value can be `null` following the common rule
below. The application only injects the public monitoring interface
through the following tokens.

```ts
class MonitoringProbe {
    constructor(
        @Inject(ZLINK_FRAMEWORK_RUNTIME)
        frameworkRuntime: ZLinkFrameworkRuntime, // the surface for object relocation, host termination, and lifecycle observation.
        @Inject(ZLINK_ROUTE_MESH_RUNTIME)
        routeMeshRuntime: ZLinkRouteMeshRuntime | null, // null if the dynamic configuration has no RouteMesh role.
        @Inject(ZLINK_CLIENT_SERVER_RUNTIME)
        clientServerRuntime: ZLinkClientServerRuntime | null, // null if the dynamic configuration has no ClientServer role.
        @Inject(ZLINK_FANOUT_RUNTIME)
        fanoutRuntime: ZLinkFanoutRuntime | null, // null if the dynamic configuration has no automatic subscriber.
    ) {}
}
```

The four providers only expose the public runtime interface — they
don't inject an internal socket monitor or private runtime object. The
fanout runtime doesn't provide a manual endpoint mutation handle.

> **`forRoot` and `forRootFactory` have different failure shapes.** In
> static `forRoot`, if a role doesn't exist, **the provider itself isn't
> registered.** In a dynamically configured path like `forRootFactory`,
> when the role doesn't exist, **the provider value can be `null`.**
> Both cases must be handled distinctly at the injection point.

**Decorator responsibility separation:**

- A **channel handler** attaches a group name with a decorator, and
  **the channel selects that group.**
- A **Spot actor handler** specifies the target Spot type with a
  decorator.
- A **Spot timer handler** is also marked with a decorator, and the
  module collects it with `zlinkDiscoverProviders(...)`.

**This separation keeps "which handler bundle a channel receives" from
mixing with "how a Spot/session processes its own internal messages."**

## 5. Lifecycle

The runtime is wired into NestJS provider lifecycle hooks.

| Hook | Timing |
|---|---|
| `onModuleInit()` | Runtime startup (bind/connect/discovery) **after every provider becomes resolvable in DI** |
| `onModuleDestroy()` | If the application shutdown hook hasn't run, starts `Shutdown` and waits for the same terminal result |
| `onApplicationShutdown()` | Joins an in-progress host termination or starts `Shutdown`, and cleans up runtime resources |

**The reason startup happens in `onModuleInit()` is that socket
bind/connect and discovery can only start once every handler provider
is resolvable.**

### 5.1 Startup Order

The lifecycle participant order is **framework → monitoring**.

1. Creates the context with the backend channel adapter.
2. Starts the MeshNode and binds the RouteMesh ROUTER.
3. Prepares location runtime and automatic connection.
4. Starts the channel receive loop and stream node.
5. Attaches the monitoring source to the ready runtime.

**Startup must be idempotent.** Even if a monitoring hook starts the
same runtime again, it isn't started twice.

### 5.2 Shutdown Order

The NestJS [shutdown](../../../01-glossary.en.md#shutdown) hook uses
the host-scoped `Shutdown`. If continuity is needed in rolling
maintenance, the operator calls the injected
`ZLinkFrameworkRuntime.relocate(...)` before the hook, confirms the
`Relocated` result, and then calls `shutdown(...)`. If relocation isn't
needed, only `shutdown(...)` is called in the hook. If `Shutdown` has
already started `Draining` when the hook starts, a new operation isn't
created — it joins that shared operation.

1. Closes new application admission at the Framework runtime's host
   maintenance barrier.
2. Processes already-accepted work and in-progress relocation/STREAM
   barriers until the deadline.
3. The Framework runtime cleans up Spot/Actor authority, descriptor,
   listener, and raw transport.
4. Closes the monitoring observer after completing the terminal result
   and events.
5. The NestJS adapter cleans up registration and backend context last.

### 5.3 Fail-Fast

**If even one component fails while building runtime state at startup,
the state built so far is disposed on the spot and the exception is
re-thrown.** A half-open socket or hanging context isn't left behind.

The internal cleanup order is owned by
[runtime-lifecycle](../../../../internals/README.en.md), and the backend
adapter port is owned by
[backend-dependency-policy](../../../../../node/internals/backend-dependency-policy.en.md).

## 6. RouteMesh Registration

Declared with the `zlinkFramework()` fluent builder.

| Role | Meaning | Bind |
|---|---|---|
| `addRouteMesh(...)` | Registers the physical MeshName and MeshNode | **Required** |
| `listen(port?)` | Opens the ROUTER listener the MeshNode shares. If omitted in automatic discovery, uses port 0 | Not required |
| `channel(name).server()` | Adds logical server membership and a handler namespace | Not required |
| `channel(name).client()` | Adds a ChannelName call role with no server [membership](../../../01-glossary.en.md#membership) | Not required |
| `addClientServerChannel(name)` | Configures a separate topology with distinct one-way request-start authority | Depends on role |
| `peerConnections()` | Adds manual peer intent with an endpoint or expected RID | Not required |
| `enablePublisher(...)` | Publishes events on this channel | **Required** |
| `enableSubscriber(...)` | Receives events on this channel | Not required |

Automatic/manual connection, dispatch key, and duplicate-check scope are
owned by
[Channel Topology §5](../../../07-channel-topology.en.md) and
[Channel Messaging](../../../08-channel-messaging.en.md).

## 7. Spot/Actor Registration

The Spot/Actor factory is registered on the owner MeshNode.
[Spot direct](../../../01-glossary.en.md#spot-direct) and Logical
Multicast use the same ROUTER as Node/Channel messaging. Discovery uses
the registered Redis location store.

| Builder | What it turns on |
|---|---|
| `addRouteMesh(meshName).listen(port?)` | The [owner](../../../01-glossary.en.md#owner) MeshNode and ROUTER listener |
| `channel(name).server()` | [Logical Multicast](../../../01-glossary.en.md#logical-multicast) scope and [handler namespace](../../../01-glossary.en.md#handler-namespace) |
| `channel(name).client()` | Outbound [ChannelName](../../../01-glossary.en.md#channelname) call with no server membership |
| `configureSpotPublisher()` | Logical Multicast's ROUTER send configuration |
| `addEntrySpot(TEntrySpot)` | The Entry Spot handler registry type |
| `addSpotFactory(TSpot)` | The Spot type this node can create |
| `addInstanceSpotFactory(type, TSpot, placement, relocation)` | The actor-free [Instance Spot](../../../01-glossary.en.md#entry-user-instance-spot) type this node can activate |
| MeshNode channel client | The client shared by a Spot handler's ChannelName send/request |

Duplicate registration and type rules are owned by
[MeshNode](../../../13-mesh-node.en.md) and
[Spot Messaging](../../../12-spot-messaging.en.md).

### 7.1 Entry Spot Identity And Membership

The framework issues the Entry Spot's global Spot ID at MeshNode
startup. The application doesn't configure or change the Entry Spot ID.
Startup initializes the Entry Spot factory and handler, finishes the
Ready barrier, and then publishes the
[descriptor](../../../01-glossary.en.md#descriptor). Actor create
finishes the selected owner MeshNode's Entry Spot membership and the
Actor [Ready](../../../01-glossary.en.md#ready) barrier in the same
lifecycle.

This order is the Framework runtime's internal responsibility. The
public interface doesn't expose a transport object, local handle, or
resolver.

The Spot message failure and Spot lifetime rules are owned by
[Spot Messaging §6](../../../12-spot-messaging.en.md#6-failure-and-lifetime).
A manual outbound peer is specified with the route mesh builder's
`connect(...)`.

### 7.2 Instance Spot Registration

The Instance Spot factory registers, together, a stable type kept
across deployments, an actor-free Spot provider, placement limit, and
relocation policy. Duplicate registration of the same
[stable type](../../../01-glossary.en.md#stable-type) or the same
provider class as a User Spot factory on the same MeshNode fails with a
configuration error before socket bind.

An Instance Spot provider can only register a direct packet and timer
handler. If an Actor handler or Logical Multicast subscription is
registered, activation fails before changing location to `Ready`. The
provider scope is cleaned up exactly once, when activation fails or the
Instance Spot closes.

Only the Instance intent of a Spot direct fluent call records the
global [Spot ID](../../../01-glossary.en.md#spot-id), stable type, and
initial Mesh as a durable creation intent. If stable type is omitted,
it's auto-selected when the selected Mesh's serving descriptor has one
distinct Instance type, and the caller specifies stable type when there
are multiple types. A regular message with no marker only takes the
Spot ID, and doesn't create an intent or start a factory for a missing
RID. The application doesn't pass a target node, owner token,
generation, or retry option.

The Ready location is observed as an immutable `SpotRef` including the
global Spot ID and exact object generation. A regular message uses Spot
ID, not ref — the exact ref is only used for close. Store version and
owner fence are kept internal to the framework and aren't delivered to
the application callback.

## 8. STREAM Registration

- **It isn't opened with decorator-based implicit registration.** Only
  explicit registration in `streams` configuration is the default
  surface.
- **One stream node only has one session.**
- **A bind endpoint must exist.**
Raw stream's `write(...)`, `close(...)` signatures are owned by
[Channel And Routing Interface](interfaces/02-channel-messaging.ko.md),
and backpressure semantics are owned by
[stream-session](../../../19-stream-session.en.md).

## 9. Session Actor Dispatch Registration

The contract is owned by
[session-actor-dispatch](../../../20-session-actor-dispatch.en.md).
When Actor dispatch is enabled on a stream node, the runtime determines
the route using the global Actor ID and current
[authority](../../../01-glossary.en.md#authority). The application
doesn't additionally register [MeshName](../../../01-glossary.en.md#meshname)
or a Spot resolver. Bound-session push is a one-way operation that only
applies to the current connection, and doesn't retarget a stale binding
to a new connection.

## 10. Monitoring/Location Registration

The contract is owned by
[runtime-monitoring](../../../24-runtime-monitoring.en.md) and
[location-runtime](../../../21-location-runtime.en.md).

| Target | Registration condition |
|---|---|
| Socket source | The name has the format `<channel>.<capability>`, and **that channel role must be registered** |
| Location source | **The polling interval must be specified.** Location runtime must be registered |
| Mesh source | Must point to a **registered MeshName** |
| Location store | Register **one physical storage instance**, **exactly once**, at the registration root. Registering it together with the memory store is a configuration error |

**Arbitrary source auto-discovery isn't supported.**

The Redis store is provided by
`@zlink-systems/framework-locations-redis` (§1).

## 11. Startup Validation

The formal source of the verification items is owned by
[Channel Messaging §9](../../../08-channel-messaging.en.md#9-verification-requirements)
and [Spot Messaging §8](../../../12-spot-messaging.en.md).

**Node throws every violation as a startup-time configuration
exception.** Surfacing configuration mistakes immediately is the
default rule.

## 12. Regression Tests

The regression items for registration and startup validation are owned
by
[regression-test-matrix](../../../../../node/internals/regression-test-matrix.en.md).
