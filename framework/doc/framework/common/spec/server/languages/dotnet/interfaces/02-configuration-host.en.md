# .NET System Structure And Host Registration

[.NET exact interface table of contents](README.en.md) · [Topology Configuration](03-configuration-topology.en.md)

## 1. Scope

This document defines the contract for registering ZLink Framework with
the ASP.NET Core host and DI. The exact signature of the RouteMesh
builder, ChannelName membership, manual peer, and runtime options is
owned by [Topology Configuration](03-configuration-topology.en.md). The
signature of handler, context, and messaging client is owned by the
per-feature documents in the
[exact interface table of contents](README.en.md).

## 2. Package Boundary

| Package | Responsibility |
|---|---|
| `Zlink.Framework` | Server application contract including handler, context, call, and [RouteMesh](../../../../01-glossary.en.md#routemesh), Spot, Actor, STREAM session, location runtime |
| `Zlink.Framework.Contracts` | Codec/error contract Server and HTTP client share |
| `Zlink.Framework.AspNetCore` | `IServiceCollection` registration and host lifecycle wiring |
| `Zlink.Framework.Codecs.Protobuf` | Optional Protobuf codec extension |
| `Zlink.Framework.Codecs.MessagePack` | Optional MessagePack codec extension |
| `Zlink.Framework.Locations.Redis` | Redis location store extension |

## 3. Host Registration

The ASP.NET Core entrypoint has the following signature.

```csharp
public static class ServiceCollectionExtensions
{
    public static IServiceCollection AddZLinkFramework(
        this IServiceCollection services,
        Action<IZLinkFrameworkOptions> configure);
    public static IHealthChecksBuilder AddZLinkDrainHealthCheck(
        this IHealthChecksBuilder builder);
    public static IServiceCollection AddZLinkHttpClient(
        this IServiceCollection services,
        string name,
        Action<ZLinkHttpClientBuilder> configure);
}
```

The framework root is registered once on one `IServiceCollection`. The
exact member of `IZLinkFrameworkOptions` is owned by
[Topology Configuration §2](03-configuration-topology.en.md#2-registration-interface).

Host startup completes normally once configuration validation and public
listener preparation finish and it can accept application callbacks. An
application callback only runs after the handler and owner queue are
ready. Hosting stop calls `IZLinkFrameworkRuntime.ShutdownAsync(...)`. If
the application needs logical continuity, it confirms the `Relocated`
result of `RelocateAsync(...)` before stop and then calls
`ShutdownAsync(...)`.

For a planned maintenance check keeping the application version,
`PlannedMaintenance` is used. This mode only selects a node whose source
and version are exactly the same as target. When switching to a prepared
new version, `RollingUpdate` and an exact target version greater than
source are specified together. If there's no eligible node for the
requested version, the framework waits until the deadline and then
returns `Blocked/TargetUnavailable`, without automatically switching to a
different version. The exact option and result types are owned by
[Host Monitoring](10-topology-monitoring.en.md).

## 4. DI Public Service

Registering the framework provides the following services on the public
DI surface.

| Service | Lifetime | Responsibility |
|---|---|---|
| `IZLinkRouteClient` | singleton | Node direct and [ChannelName](../../../../01-glossary.en.md#channelname) send/request |
| `IZLinkSpotClient` | singleton | Global SpotId direct send/request and explicit Instance cold activation |
| `IZLinkSpotManager` | singleton | User Spot creation, resolve, and exact close |
| `IZLinkSpotPublisherClient` | singleton | [Spot](../../../../01-glossary.en.md#spot) Logical Multicast publish |
| `IZLinkFanoutClient` | singleton | Publish typed events to a classic fanout ChannelName |
| `IZLinkActorClient` | singleton | Global ActorId direct send/request |
| `IZLinkActorManager` | singleton | Actor creation, resolve, and close |
| `IZLinkRouteMeshRuntimeOptions` | singleton | Query/set Mesh placement weight and ChannelName [weight](../../../../01-glossary.en.md#weight) |
| `IZLinkFrameworkRuntime` | singleton | Host state, readiness, `Relocate`, and `Shutdown` |
| `IZLinkRouteMeshRuntime` | singleton | RouteMesh operational status |
| `IZLinkClientServerRuntime` | singleton | ClientServer Channel operational status |
| `IZLinkFanoutRuntime` | singleton | Automatic fanout Channel operational status |

Querying an unregistered MeshName or runtime capability raises
`ZLinkConfigurationException`. An unregistered ChannelName for Channel
send/request completes with `NotFound`. A Spot handler is created in the
Spot activation scope, and an Actor handler in the Actor activation
scope. Handler types aren't resolved directly from DI — only constructor
dependencies are resolved in that scope. When a handler uses a service,
it uses constructor injection, not the context as a service locator. The
detailed lifetime follows the [Spot Interface](05-spots.en.md).

## 5. Location Store Registration

A host using automatic discovery, distributed Spot/Actor address,
Instance Spot activation, or Actor relocation explicitly registers an
`IZLinkLocationStore` implementation on the root. The code below is an
example using the official Redis provider the framework provides. The
application can also register a different provider implementing the same
public interface.

```csharp
services.AddZLinkFramework(options =>
{
    options.AddLocationStore(
        new ZLinkRedisLocationStore(redisOptions)); // provides atomic batch on opaque location records.
    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(relocationOptions)); // holds immutable relocation payload as a separate capability.
});
```

A Redis-specific registration helper isn't provided. The root's
`AddLocationStore(...)` and `AddRelocationStore(...)` each take one
interface instance. The Location instance provides exact read,
conditional atomic batch, and bounded snapshot scan. The Relocation
instance stores an immutable payload at a framework-issued reference. One
instance implementing both capabilities together isn't provided as the
official Redis contract.

A [MeshNode](../../../../01-glossary.en.md#meshnode) that only uses manual
peers and doesn't use distributed location features can start without a
[location store](../../../../01-glossary.en.md#location-store). Manual
peers also pass [MeshName](../../../../01-glossary.en.md#meshname), RID,
lifecycle generation, ChannelName set, and security identity admission.

## 6. Codec

A typed handler and client exchange business objects. Since the
framework provides a JSON serializer by default, there's no
message-specific registration API for using JSON. Protobuf, MessagePack,
and a custom codec are each registered once on the root's codec registry
as an extension.

The codec is only responsible for converting between payload and
business object. Packet name, metadata, routing, and reply correlation
are owned by the framework. The
[packet name](../../../../01-glossary.en.md#packet-name) is determined
in the handler registration descriptor, and changing the codec doesn't
change the dispatch key.

## 7. Startup Validation

The host validates the following conditions before network bind.

- Duplicate framework root and MeshName
- The MeshNode's [Routing ID](../../../../01-glossary.en.md#routing-id)
  and listener configuration. Server
  [membership](../../../../01-glossary.en.md#membership) can be 0
- ChannelName's `Client()`/`Server()` role and process-local topology
  duplication
- The location store needed for ClientServer automatic discovery
- The connectable AdvertiseHost when using a wildcard BindHost
- Duplicate handler key in the same owner namespace
- Owner relationship of Spot, Actor, and STREAM factory
- Duplicate Object role selection, Location Store registration for
  Client/Server role, and absence of a
  [factory](../../../../01-glossary.en.md#factory) for None role
- Duplicate stable type/implementation class for Actor/User Spot/
  [Instance Spot](../../../../01-glossary.en.md#entry-user-instance-spot),
  explicit relocation policy, and per-type capacity
- Node placement weight and active/pending capacity
- Host `ApplicationVersion` range and `MaintenanceWave` format
- Whether `PreserveStateWith`'s Actor/Spot adapter type matches its
  factory target
- Whether exactly one Relocation Store is registered when there's any
  `RecreateOnRelocation` or `PreserveStateWith` factory, or any Instance
  Spot factory
- The store instance needed for automatic discovery or distributed
  location features
- An invalid combination of
  [automatic discovery](../../../../01-glossary.en.md#automatic-discovery)/
  object role and fixed routing ID, and RID prefix format
- TLS certificate, key, and trust configuration

A validation failure fails host startup with
`ZLinkConfigurationException`. Since the runtime isn't created on the
first call, a configuration error doesn't first appear while processing a
message.

## 8. Runtime Option

The public runtime option for Mesh placement weight and Channel weight is
owned by
[Topology Configuration §5](03-configuration-topology.en.md#5-publisher-and-runtime-option).
At runtime, node placement weight can be set by MeshName, and a local
Server's `Weight` by ChannelName. The two values apply to different
selections. Transport options including `MaxMessageSize` are only set
before startup, and a runtime setter isn't provided.

Framework service liveness is fixed to a profile that sends a probe
every 5 seconds regardless of application traffic and must receive the
matching ACK on the same current connection within 15 seconds. A
different inbound frame doesn't satisfy the ACK deadline. A C# public
option to change this value isn't provided, and it isn't treated as the
same setting as the owner lease renew interval.

A [Logical Multicast](../../../../01-glossary.en.md#logical-multicast)
publisher doesn't provide a publish-only delivery policy option. Each
remote target follows the MeshNode ROUTER's HWM and send timeout, and the
local Spot queue accepts or drops independently.
