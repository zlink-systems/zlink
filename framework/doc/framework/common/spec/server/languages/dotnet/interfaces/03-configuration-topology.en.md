# .NET RouteMesh/MeshNode Public Interface

[.NET exact interface table of contents](README.en.md) · [Common Topology](../../../../07-channel-topology.en.md) ·
[MeshNode](../../../../13-mesh-node.en.md) · [Message Model](../../../../04-message-model.en.md)

## 1. Scope

This document fixes ZLink Framework's .NET RouteMesh/MeshNode public
interface. The target audience is .NET application developers and public
provider implementers. This document owns the exact C# signature for
physical mesh registration, logical channel membership, manual peer,
handler, Spot/Actor registration, and runtime weight change.

## 2. Registration Interface

```csharp
public interface IZLinkFrameworkOptions
{
    TimeSpan DefaultRequestTimeout { get; set; }
    TimeSpan DefaultSocketSendTimeout { get; set; }
    long ApplicationVersion { get; set; }
    string? MaintenanceWave { get; set; }
    IZLinkCodecRegistryBuilder Codecs { get; }
    IZLinkWorkerOptions Worker { get; }

    void AddHandlersFromAssemblyOf<TMarker>();
    void AddHandlersFromAssemblyOf(Type markerType);
    void AddHandlersFromAssembly(System.Reflection.Assembly assembly);
    void DisableImplicitHandlerAutoRegistration();
    IZLinkMetadataPolicyBuilder ConfigureMetadata();
    void AddLocationStore(IZLinkLocationStore store);
    void AddRelocationStore(IZLinkRelocationStore store);
    ZLinkLocationOptions ConfigureLocations();
    IZLinkInboundDispatchOptions ConfigureInboundDispatch();
    IZLinkNetworkOptions ConfigureNetwork();
    IZLinkDispatchOptions ConfigureDispatch();
    IZLinkStreamCompressionBuilder ConfigureStreamCompression();
    void UseFilter<TFilter>() where TFilter : class, IZLinkHandlerFilter;

    IZLinkMeshNodeBuilder AddRouteMesh(string meshName);
    IZLinkClientServerChannelRoleBuilder AddClientServerChannel(string channelName);
    IZLinkFanoutChannelBuilder AddFanoutChannel(string channelName);
    IZLinkStreamNodeBuilder AddStreamNode(string streamNodeName);
}

public enum ZLinkApplicationHwmProfile
{
    Compact = 0,
    LowLatency = 1,
    Balanced = 2,
    Throughput = 3
}

public interface IZLinkInboundDispatchOptions
{
    ulong? ApplicationHwmBytes { get; set; }
    ZLinkApplicationHwmProfile ApplicationHwmProfile { get; set; }
    ulong? ProcessMemoryLimitBytes { get; set; }
}

public interface IZLinkMeshNodeBuilder
{
    IZLinkMeshChannelRoleBuilder Channel(string channelName);
    IZLinkMeshNodeBuilder Listen(string endpoint);
    IZLinkMeshNodeBuilder Listen(int port = 0);
    IZLinkMeshNodeBuilder SetBindHost(string bindHost);
    IZLinkMeshNodeBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkMeshNodeBuilder SetRoutingId(RoutingId routingId);
    IZLinkMeshNodeBuilder SetRoutingIdPrefix(string prefix);
    IZLinkMeshNodeBuilder SetPlacementWeight(int weight);
    IZLinkMeshNodeBuilder SetActorLimit(int limit);
    IZLinkMeshNodeBuilder SetSpotLimit(int limit);
    IZLinkMeshNodeBuilder SetActivationConcurrency(int limit);
    IZLinkMeshNodeBuilder SetInstanceSpotIdleTimeout(TimeSpan timeout);
    IZLinkMeshObjectRoleBuilder Objects();
    IZLinkMeshNodeSocketConfig ConfigureRouterSocket();
    IZLinkSpotPublisherConfig ConfigureSpotPublisher();
    IZLinkMeshPeerConnections PeerConnections { get; }

    IZLinkMeshNodeBuilder SetDefaultRequestTimeout(TimeSpan timeout);
    IZLinkMeshNodeBuilder AddRouteSendHandler<THandler, TMessage>(
        string? packetName = null)
        where THandler : class, IZLinkRouteSendHandler<TMessage>;
    IZLinkMeshNodeBuilder AddRouteSendHandler<THandler>(string? packetName = null)
        where THandler : class;
    IZLinkMeshNodeBuilder AddRouteRequestHandler<THandler, TRequest, TReply>(
        string? packetName = null)
        where THandler : class, IZLinkRouteRequestHandler<TRequest, TReply>;
    IZLinkMeshNodeBuilder AddRouteRequestHandler<THandler>(string? packetName = null)
        where THandler : class;

}

public interface IZLinkMeshObjectRoleBuilder
{
    IZLinkMeshObjectClientBuilder Client();
    IZLinkMeshObjectServerBuilder Server();
}

public interface IZLinkMeshObjectClientBuilder
{
}

public interface IZLinkMeshObjectServerBuilder
{
    IZLinkMeshObjectServerBuilder AddEntrySpot<TEntrySpot>()
        where TEntrySpot : class, IZLinkEntrySpot;
    IZLinkMeshObjectServerBuilder AddSpotFactory<TSpot>(
        string spotType,
        Action<IZLinkUserSpotFactoryBuilder<TSpot>> configure)
        where TSpot : class, IZLinkSpot;
    IZLinkMeshObjectServerBuilder AddInstanceSpotFactory<TSpot>(
        string instanceSpotType,
        Action<IZLinkInstanceSpotFactoryBuilder<TSpot>> configure)
        where TSpot : class, IZLinkInstanceSpot;
    IZLinkMeshObjectServerBuilder AddActorFactory<TActor, TFactory>(
        string actorType,
        Action<IZLinkActorFactoryBuilder<TActor>> configure)
        where TActor : class, IZLinkActor
        where TFactory : class, IZLinkActorFactory<TActor>;
}

public enum ZLinkUserSpotExecutionMode
{
    SpotWide = 0,
    PerActor = 1
}

public enum ZLinkSpotRelocationReadinessMode
{
    AnyTurnBoundary = 0,
    ApplicationSignaled = 1
}

public interface IZLinkActorFactoryBuilder<TActor>
    where TActor : class, IZLinkActor
{
    IZLinkActorFactoryBuilder<TActor> DisableRelocation();
    IZLinkActorFactoryBuilder<TActor> RecreateOnRelocation();
    IZLinkActorFactoryBuilder<TActor> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkActorRelocationAdapter<TActor>;
}

public interface IZLinkUserSpotFactoryBuilder<TSpot>
    where TSpot : class, IZLinkSpot
{
    IZLinkUserSpotFactoryBuilder<TSpot> StableTypeLimit(int limit);
    IZLinkUserSpotFactoryBuilder<TSpot> ExecutionMode(
        ZLinkUserSpotExecutionMode mode);
    IZLinkUserSpotFactoryBuilder<TSpot> RelocationReadiness(
        ZLinkSpotRelocationReadinessMode mode);
    IZLinkUserSpotFactoryBuilder<TSpot> DisableRelocation();
    IZLinkUserSpotFactoryBuilder<TSpot> RecreateOnRelocation();
    IZLinkUserSpotFactoryBuilder<TSpot> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkSpotRelocationAdapter<TSpot>;
}

public interface IZLinkInstanceSpotFactoryBuilder<TSpot>
    where TSpot : class, IZLinkInstanceSpot
{
    IZLinkInstanceSpotFactoryBuilder<TSpot> StableTypeLimit(int limit);
    IZLinkInstanceSpotFactoryBuilder<TSpot> DisableRelocation();
    IZLinkInstanceSpotFactoryBuilder<TSpot> RecreateOnRelocation();
    IZLinkInstanceSpotFactoryBuilder<TSpot> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkSpotRelocationAdapter<TSpot>;
}

public interface IZLinkNetworkOptions
{
    string BindHost { get; set; }
    string? AdvertiseHost { get; set; }
}

public interface IZLinkMeshChannelRoleBuilder
{
    IZLinkMeshChannelClientBuilder Client();
    IZLinkMeshChannelServerBuilder Server();
}

public interface IZLinkMeshChannelClientBuilder
{
}

public interface IZLinkMeshChannelServerBuilder
{
    IZLinkMeshChannelServerBuilder SetWeight(int weight);
    IZLinkMeshChannelServerBuilder AddHandlerGroup(string groupName);
    IZLinkMeshChannelServerBuilder AddSendHandler<THandler, TMessage>(
        string? packetName = null)
        where THandler : class, IZLinkSendHandler<TMessage>;
    IZLinkMeshChannelServerBuilder AddSendHandler<THandler>(string? packetName = null)
        where THandler : class;
    IZLinkMeshChannelServerBuilder AddRequestHandler<THandler, TRequest, TReply>(
        string? packetName = null)
        where THandler : class, IZLinkRequestHandler<TRequest, TReply>;
    IZLinkMeshChannelServerBuilder AddRequestHandler<THandler>(string? packetName = null)
        where THandler : class;
}

public interface IZLinkClientServerChannelRoleBuilder
{
    IZLinkClientServerChannelClientBuilder Client();
    IZLinkClientServerChannelServerBuilder Server();
}

public interface IZLinkClientServerChannelClientBuilder
{
    IZLinkClientServerChannelClientBuilder Connect(string endpoint);
}

public interface IZLinkClientServerChannelServerBuilder
{
    IZLinkClientServerChannelServerBuilder Listen(int port = 0);
    IZLinkClientServerChannelServerBuilder SetBindHost(string bindHost);
    IZLinkClientServerChannelServerBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkClientServerChannelServerBuilder SetWeight(int weight);
    IZLinkClientServerChannelServerBuilder AddHandlerGroup(string groupName);
    IZLinkClientServerChannelServerBuilder AddSendHandler<THandler, TMessage>(
        string? packetName = null)
        where THandler : class, IZLinkSendHandler<TMessage>;
    IZLinkClientServerChannelServerBuilder AddRequestHandler<THandler, TRequest, TReply>(
        string? packetName = null)
        where THandler : class, IZLinkRequestHandler<TRequest, TReply>;
}

public interface IZLinkEndpointConnections
{
    void Connect(string endpoint);
    void Disconnect(string endpoint);
    IReadOnlyList<string> ListConnections();
}

public interface IZLinkFanoutChannelBuilder
{
    IZLinkFanoutChannelBuilder EnablePublisher(string endpoint);
    IZLinkFanoutChannelBuilder EnablePublisher(int port = 0);
    IZLinkFanoutChannelBuilder SetBindHost(string bindHost);
    IZLinkFanoutChannelBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkFanoutChannelBuilder SetRoutingId(RoutingId publisherRoutingId);
    IZLinkFanoutChannelBuilder SetRoutingIdPrefix(string prefix);
    IZLinkFanoutChannelBuilder EnableSubscriber();
    IZLinkFanoutChannelBuilder Connect(string endpoint);
    IZLinkEndpointConnections SubscriberConnections { get; }
    IZLinkFanoutChannelBuilder AddHandler<THandler, TEvent>(
        string? packetName = null)
        where THandler : class, IZLinkFanoutHandler<TEvent>;
}

public interface IZLinkStreamNodeBuilder
{
    IZLinkStreamNodeBuilder Bind(string endpoint);
    IZLinkStreamNodeBuilder Bind(int port = 0);
    IZLinkStreamNodeBuilder SetBindHost(string bindHost);
    IZLinkStreamNodeBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkSocketConfig ConfigureSocket();
    IZLinkStreamNodeBuilder EnableActorDispatch();
    IZLinkStreamNodeBuilder SetTlsServer(
        string certificatePath,
        string keyPath,
        bool requireClientCertificate = false);
    IZLinkStreamNodeBuilder AddSession<TSession>()
        where TSession : class, IZLinkSession;
}

public interface IZLinkStreamCompressionBuilder
{
    IZLinkStreamCompressionBuilder UseDefault();
    IZLinkStreamCompressionBuilder UseLz4();
    IZLinkStreamCompressionBuilder Use(IZlinkStreamCompressionCodec codec);
    IZLinkStreamCompressionBuilder Disable();
}

public interface IZLinkMetadataPolicyBuilder
{
    IZLinkMetadataPolicyBuilder AllowSessionToActor(string key);
    IZLinkMetadataPolicyBuilder AllowActorToSession(string key);
}

```

`IZLinkStreamNodeBuilder.ConfigureSocket().MaxMessageSize` defaults to
`64 KiB`. It is used only when a StreamNode's Core STREAM inbound path checks
a complete client-to-server message, measured as header bytes plus payload
bytes and excluding the 6-byte prefix. `0` maps to Core `-1`, so Framework
adds no limit; a negative value is a startup configuration error. A message
over the limit is never partly delivered to the handler. The server records
`EMSGSIZE` and a diagnostic trace, then closes the connection. The raw client
observes the close rather than a separate wire error code. The Framework
limit doesn't apply to server-to-client outbound messages. ClientServer and
RouteMesh SS don't gain this setting.

The exact declaration of `IZLinkCodecRegistryBuilder` and the codec
extension is owned by [Serialization](11-serialization.ko.md).

`AddRouteMesh(meshName)` registers one process-local
[MeshNode](../../../../01-glossary.en.md#meshnode). Registering the same
`meshName` twice in the same process fails host startup with
`ZLinkConfigurationException`. After `Channel(channelName)`, call exactly
one of `Client()` or `Server()`. `Client()` only creates the send path,
and only `Server()` provides
[weight](../../../../01-glossary.en.md#weight) and handler registration.
A MeshNode with no Server [membership](../../../../01-glossary.en.md#membership)
can also start.

An automatic [RouteMesh](../../../../01-glossary.en.md#routemesh)
compares RID in canonical byte order, and only the MeshNode with the
smaller RID connects to the counterpart endpoint. Connection intent isn't
created only when both local and remote object roles are `Client` and
neither has RouteMesh Channel Server membership. Channel Client
membership alone doesn't connect. If either side has Channel Server
membership, connection is needed even if weight is `0`. A manual
topology can connect from one or both sides depending on application
endpoint configuration. If bidirectional connection or automatic
discovery contention/a stale snapshot creates a duplicate candidate,
handshake and admission check the same RID and lifecycle generation and
keep only one in ready state.

If a manual endpoint's remote object role and RouteMesh Server membership
can't be known before connect, they're confirmed in the handshake. Only
when both sides are Object Client and neither has RouteMesh Channel
Server membership does admission end with a `NotRequired` terminal and
close the socket before ready. Background reconnect isn't repeated for
the same endpoint and configuration generation. If the endpoint, expected
RID, or configuration generation changes, it's re-confirmed once as a new
intent.

`Listen(string endpoint)`, `Bind(string endpoint)`, and
`EnablePublisher(string endpoint)` are provided, and the host/port
combination overload expresses the same listener configuration.

`AddClientServerChannel(channelName)` can register `Client()` and
`Server()`, either one or both, and each role is registered at most
once. The registration key is `(ChannelName, Role)`, and Client and
Server share one ClientServer topology through separate registrations.
Registering the same role twice fails startup. The RouteMesh ChannelName
conflict rule stays the same. A Client can use both the registered manual
endpoint and the server endpoint of the same
[ChannelName](../../../../01-glossary.en.md#channelname) automatically
discovered from the location store as connection targets. If the two
sources point to the same Server RID and
[lifecycle generation](../../../../01-glossary.en.md#lifecycle-generation),
the connection intent and ready target are merged into one. In both
automatic and manual, only Client connects to server — Server doesn't
look for a client endpoint or start an outbound connect. Server only
provides the received send/request handler and request reply, and
doesn't start a new business call to a connected client.

If a Server role is also registered on the same process, a local Server
that finished listener and service admission is put in the same
candidate set as a remote Server. The same
[Ready](../../../../01-glossary.en.md#ready), positive weight, and
non-draining conditions apply, with no local priority or remote
exclusion rule. After selection, the actual transport message is
delivered from the Client DEALER to the Server ROUTER, without calling
the handler directly.

`ConfigureNetwork()`'s default BindHost is `127.0.0.1`, and if
AdvertiseHost is omitted, a non-wildcard
[BindHost](../../../../01-glossary.en.md#bindhost) is used. An
[automatic discovery](../../../../01-glossary.en.md#automatic-discovery)
listener binds to port `0` if the port on `Listen()`/`Bind()`/
`EnablePublisher()` is omitted, or if the listener call itself is
omitted. In manual mode, if the endpoint can't be obtained from a
different discovery source, the listen port and remote endpoint are
specified explicitly. A per-listener host setting takes priority over the
root default.

A fanout publisher that registered a
[location store](../../../../01-glossary.en.md#location-store) has the
framework generate a per-lifecycle RID and publish a dedicated
descriptor. A publisher with no Store can still be used as a target with
a fixed RID and manually delivered listener endpoint. `EnableSubscriber()`,
which takes no endpoint, discovers every valid publisher of the same
ChannelName from the location store. `Connect(endpoint)` configures a
manual subscriber that only uses the specified endpoint. Configuring both
an automatic subscriber and a manual subscriber on one fanout channel
fails startup. An automatic subscriber needs a location store, but it
isn't needed for a host that only uses a manual publisher and manual
subscriber. A publisher only publishes a
[descriptor](../../../../01-glossary.en.md#descriptor) and doesn't start
an outbound connect to a subscriber endpoint. Only the subscriber
connects to the publisher endpoint, and an automatic subscriber creates
one connection intent per Publisher RID and lifecycle generation.

Automatic RID has the format `prefix-<lowercase-canonical-uuid-v4>`.
UUID v4 is represented as a lowercase canonical string in `8-4-4-4-12`
digit groups. Prefix is ASCII `[A-Za-z0-9._-]` 1..64 characters, and the
full RID is at most 255 UTF-8 bytes. On conflict with an active owner, it
fails immediately with `RoutingIdConflict` instead of retrying with a
new UUID. Fixed `SetRoutingId(...)` is only allowed in a manual topology
with no object role and no Store descriptor. Slot count, allocation
group, and a public allocation provider aren't provided.

The Object Server's Entry Spot ID also uses the same prefix, but with a
UUID v4 generated separately from the MeshNode RID attached. The format
is `<prefix>-entry-<lowercase-canonical-uuid-v4>`, and the caller doesn't
specify a fixed Entry Spot ID. This ID's global conflict and the reserved
format validation for a caller-specified Spot ID are defined by the
[Spot Model](../../../../11-spot-model.en.md). The prefix and the
generated RID/Spot ID aren't interpreted as placement, shard, or stable
application identity.

A registered MeshNode descriptor must be at most 1 MiB. The
[Spot](../../../../01-glossary.en.md#spot) type and stateful object
capability collection are each at most 1024. Exceeding the bound fails
startup — it doesn't apply only some of the registrations.

`SubscriberConnections` is a runtime handle for the manual subscriber
endpoint set. It provides connect, disconnect, and current-list query
targeting the same set of endpoints registered on the builder. An
automatic subscriber's discovery results aren't changed by this handle.

`AddHandlersFromAssemblyOf(...)` and `AddHandlersFromAssembly(...)` only
add the specified assembly to the handler scan scope. The exact
declaration of the method, group, and packet attributes used for the
scan is owned by [Common Runtime](01-common-runtime.ko.md).

`EnableActorDispatch()` only activates a STREAM node's Actor dispatch
capability. If the same host has no Mesh whose object role is `Client`
or `Server`, and no Location Store, startup fails. Since the global
ActorId determines the current Mesh and owner route, this setting doesn't
take a MeshName.

`DefaultRequestTimeout`'s default is 30 seconds, and
`DefaultSocketSendTimeout`'s default is 1 second. `Worker` sets the
worker's minimum/maximum thread count, idle timeout, and queue cap before
host startup.

`ConfigureStreamCompression()` and `IZLinkStreamCompressionBuilder` pick
the STREAM payload compression. This builder doesn't configure the
service transport lifecycle or a relocation codec.

`ApplicationVersion` is set once for the whole host, in the range
`0..long.MaxValue`, defaulting to `0`. Every local MeshNode publishes
this value, and a negative value is rejected with
`ZLinkConfigurationException` before startup. `MaintenanceWave` is a
stable ID that, when `null`, means no wave exclusion is used.

The object role of a MeshNode that didn't call `Objects()` is `None`.
`Client()` provides a manager and an ID-only message client, but doesn't
become a placement target. `Server()` includes Client capability and
registers Entry Spot and factory. Both roles require a Location Store.
The role can only be selected once.

A MeshNode that selected `Objects().Client()` can also register
`Channel(...).Server()`. This combination needs a peer connection to
process Channel request/send. Even if Server weight is `0`, Server
capability and the need for connection are kept — it's only excluded
from the selection candidates of a new Channel operation. An application
Node direct handler such as `AddRouteSendHandler(...)`/
`AddRouteRequestHandler(...)` can't be registered on an Object Client and
fails with `ZLinkConfigurationException` before socket bind.

Between two Object Clients, an automatic or manual peer connection isn't
needed only when neither side has RouteMesh Channel Server membership.
The same applies when only Channel Client membership is registered. If
either side has RouteMesh Channel Server membership, the connection is
kept. ClientServer and classic fanout are separate physical topologies,
so they aren't included in this judgment. A connection between an Object
Client and Object Server, or between Object Servers, is kept.

The Actor/User Spot/Instance Spot
[factory](../../../../01-glossary.en.md#factory) fixes stable type,
per-object-kind factory options, and explicit relocation policy in the
same registration. There's no overload that omits the policy.
[Stable type](../../../../01-glossary.en.md#stable-type) is UTF-8 1..255
bytes, and a duplicate type is a startup error. The Entry Spot ID is
issued by the framework.

Node placement weight is 0..10000, defaulting to 100. An out-of-range
value is `ZLinkConfigurationException` in both startup config and
runtime change. The default `0` for Actor/Spot population limit means no
limit, and the default for pending activation concurrency is 128. If a
per-type limit is `null`, it shares the node limit; if it has a value, it
must be 1..`int.MaxValue`, and a value smaller than the node limit
applies. Capacity is applied before weight, and if there's no eligible
node, it's `CapacityExceeded`.

`SetInstanceSpotIdleTimeout(...)` is the reference time for cleaning up
an idle Instance Spot. The default is `TimeSpan.Zero`, and
`TimeSpan.Zero` means no cleanup. The allowed range is `TimeSpan.Zero`
and positive values — a negative value is `ZLinkConfigurationException`
before startup. The value is fixed before the MeshNode lifecycle starts,
and a runtime setter isn't provided. It's a separate setting from
`ZLinkWorkerOptions.IdleTimeout`, and they don't inherit each other's
value. Only Instance Spot is a cleanup target — Entry Spot and User Spot
aren't affected by this setting. The idle judgment condition, the
delivery of `ZLinkSpotCloseReason.IdleEvicted`, and the cold-activation
rule after cleanup are owned by
[Spot Model §6.2](../../../../11-spot-model.en.md#62-cleaning-up-an-idle-instance-spot).

## 3. Manual Peer

```csharp
public readonly record struct ZLinkMeshPeerConnection(
    string Endpoint,
    RoutingId? ExpectedRoutingId);

public interface IZLinkMeshPeerConnections
{
    void Connect(string endpoint);
    void Connect(RoutingId expectedRoutingId, string endpoint);
    void Disconnect(string endpoint);
    IReadOnlyList<ZLinkMeshPeerConnection> ListConnections();
}
```

If both MeshNodes specified via `Connect(...)` are Object Client and
neither has RouteMesh Channel Server membership, the configuration intent
can remain in the list but doesn't become a ready peer. Once handshake
ends with `NotRequired`, it doesn't reconnect for the same configuration
generation, and isn't included in the public RouteMesh status's ready
peer count or liveness targets. If either side has Channel Server
membership, including weight `0`, the regular peer admission and
liveness rule applies.

A handler filter is a public extension point the application implements
and registers on the root. Calling `next` runs the remaining filters and
handler. A request that doesn't call it ends with `Rejected`, and the
filter doesn't build a business reply directly. The applicable scope,
execution order, and fanout isolation are determined by the common
Framework API.

```csharp
public enum ZLinkHandlerDispatchKind
{
    NodeDirectSend = 0,
    NodeDirectRequest = 1,
    ChannelSend = 2,
    ChannelRequest = 3,
    ClassicFanout = 4
}

public interface IZLinkHandlerFilterContext : IZLinkMessageContext
{
    ZLinkHandlerDispatchKind DispatchKind { get; }
}

public delegate ValueTask ZLinkHandlerFilterNext();

public interface IZLinkHandlerFilter
{
    ValueTask InvokeAsync(
        IZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext next,
        CancellationToken cancellationToken);
}
```

`ChannelSend` and `ChannelRequest` include both RouteMesh and
ClientServer. The RouteMesh and Node direct context provides MeshName,
and ClientServer and `ClassicFanout` provide `null`. A filter calls
`next` at most once. A second call fails with
`ZLinkFrameworkErrorKind.InvalidOperation` and doesn't re-run the
handler. If `next` isn't called on a request, a
`ZLinkFrameworkErrorKind.Rejected` reply is sent. Behavior where a filter
substitutes for the business reply isn't provided via a compatibility
overload or adapter.

`AddInstanceSpotFactory`'s type name can't be empty and must be at most
255 UTF-8 bytes. Per-type active and pending limits can be omitted, but
an explicit value is 1..`int.MaxValue`. On the same MeshNode, the same
stable type or the same implementation class can't be duplicated across
a User Spot factory and an Instance factory. If `TSpot` also implements
the closed generic `IZLinkUserSpotActorLifecycle<TActor>`, it conflicts
with the actor-free contract, so startup fails. Both options apply per
local MeshNode and per Instance type. The registered type set is fixed
before the descriptor is first published, and doesn't change after
startup.

The factory configure callback sets options and relocation policy on one
builder. The callback must call exactly one of `DisableRelocation()`,
`RecreateOnRelocation()`, `PreserveStateWith<TAdapter>()`. Selecting
none or more than one is a startup configuration error before socket
bind. The Actor builder only takes
`IZLinkActorRelocationAdapter<TActor>`, and the User/Instance Spot
builder only takes `IZLinkSpotRelocationAdapter<TSpot>`. If the factory
target and adapter kind don't match, it fails with the same error.

The framework synchronously runs the callback exactly once inside the
registration call. Once the callback returns, the builder configuration
is fixed. If the application keeps the builder outside the callback and
calls it again, it's a configuration error. If the callback throws, the
factory isn't registered and the same exception is propagated to the
caller.

A User Spot that selected `ZLinkUserSpotExecutionMode.PerActor` only
allows `RecreateOnRelocation()`. Registering `DisableRelocation()` or
`PreserveStateWith<TAdapter>()` together is a startup configuration
error before socket bind. A PerActor Spot is a stateless execution
shell, and the member Actor's relocation policy and adapter each handle
Actor state. Shared state and Spot-level schedules that must be kept are
placed in an external store the application owns, such as Redis or a
database.

Execution mode defaults to `SpotWide`, and relocation readiness defaults
to `AnyTurnBoundary`. `ApplicationSignaled` is only allowed with
`SpotWide`. Registering it together with `PerActor` is a startup
configuration error before socket bind. The callback uses `IZLinkSpot`'s
default no-op implementation, so an application override isn't required.

If expected RID is omitted, the admission handshake determines the
remote identity. If expected RID is specified, the connection isn't
admitted if the handshake identity differs. A manual connection also uses
the same [MeshName](../../../../01-glossary.en.md#meshname)/RID/
ChannelName/security validation as an automatic discovery connection.

## 4. Dispatch Scope Of Handler And Filter

A DI scope is created each time a Node direct/Channel send/request and
classic fanout subscription handler runs. The handler and filter are
each created once by the framework in this scope, and use the same
scoped dependency. If a classic fanout message matches multiple
subscription handlers, a separate scope is created per subscription
handler. Even if the application registers the handler or filter type as
singleton/scoped/transient, this lifetime doesn't change. Once dispatch
finishes, the framework cleans up the instances it created first, then
cleans up the scope.

A Channel handler is distinguished by `(ChannelName, message kind,
packet name)`. A RID direct route handler is registered on the MeshNode
builder and uses a route handler context that provides source RID.
Duplicate registration of the same key is a startup error, and the same
packet name can be registered across different channels or route
families.

`AddHandlerGroup(groupName)` exposes, on that ChannelName, the send/
request handlers found by scanning that have the same
`ZLinkHandlerGroupAttribute` value. Use typed `AddSendHandler(...)`/
`AddRequestHandler(...)` directly only for a case that demonstrates
manual registration, like TicTacToe.

The weight of `IZLinkMeshChannelServerBuilder` and
`IZLinkClientServerChannelServerBuilder` is 0 to 10000, defaulting to
100. An out-of-range value is `ZLinkConfigurationException` in both
startup config and runtime change. Weighted selection, including node
placement, computes the sum of candidate weight using at least a 64-bit
integer. 0 is only excluded from that channel's new select-one and a
RouteMesh Logical Multicast remote target. It doesn't affect a RID
direct route, other membership, or an already-submitted operation.

## 5. Publisher And Runtime Option

```csharp
public interface IZLinkSpotPublisherConfig
{
    ulong SendHighWaterMark { get; set; }
    TimeSpan? SendTimeout { get; set; }
    TimeSpan? Linger { get; set; }
}

public interface IZLinkSpotSubscriberConfig
{
    ulong ReceiveHighWaterMark { get; set; }
    TimeSpan? ReceiveTimeout { get; set; }
    TimeSpan? Linger { get; set; }
}

public interface IZLinkSocketConfig
{
    long MaxMessageSize { get; set; }
    ulong SendHighWaterMark { get; set; }
    ulong ReceiveHighWaterMark { get; set; }
    int SendBufferSize { get; set; }
    int ReceiveBufferSize { get; set; }
    TimeSpan? Linger { get; set; }
    TimeSpan? ReceiveTimeout { get; set; }
    TimeSpan? SendTimeout { get; set; }
    TimeSpan? ConnectTimeout { get; set; }
    TimeSpan? HandshakeInterval { get; set; }
    bool IPv6 { get; set; }
    bool TcpNoDelay { get; set; }
    bool Immediate { get; set; }
    int Weight { get; set; }
}

public interface IZLinkRouteConfig
{
    bool RequireKnownPeer { get; set; }
    bool AllowPeerHandover { get; set; }
    bool EnablePeerProbe { get; set; }
    RoutingId ConnectRoutingId { get; set; }
}

public interface IZLinkOutboundRouteConfig
{
    bool ProbeRouterOnConnect { get; set; }
}

public interface IZLinkRouteMeshRuntimeOptions
{
    IZLinkMeshPlacementRuntimeOptions Mesh(string meshName);
    IZLinkMeshChannelRuntimeOptions Channel(string channelName);
}

public interface IZLinkMeshPlacementRuntimeOptions
{
    int PlacementWeight { get; set; }
}

public interface IZLinkMeshChannelRuntimeOptions
{
    int Weight { get; set; }
}

public interface IZLinkMeshNodeSocketConfig
{
    long MaxMessageSize { get; set; }
    ulong SendHighWaterMark { get; set; }
    ulong ReceiveHighWaterMark { get; set; }
    ulong MailboxMessageBudget { get; set; }
    ulong MailboxByteBudget { get; set; }
    TimeSpan? ReceiveTimeout { get; set; }
    TimeSpan? SendTimeout { get; set; }
}
```

The application listener's default `MaxMessageSize` is `16 MiB`.
Specifying `0` while using Application HWM as Auto or a positive value is
a startup configuration error. `MaxMessageSize = 0` can only be used when
`ApplicationHwmBytes = 0`.

`ConfigureSpotPublisher()` doesn't provide a publish-only delivery
policy option. [Logical Multicast](../../../../01-glossary.en.md#logical-multicast)
starts once it secures source-local execution capacity within the send
timeout, and completes normally with no return value. It doesn't wait
for or aggregate per-target admission/failure results into public
monitoring, and doesn't automatically retry the whole publish due to
some target's failure. It completes normally even with no targets.

`IZLinkRouteMeshRuntimeOptions` is a public DI singleton. Querying
unregistered membership is `ZLinkConfigurationException`.
`MailboxMessageBudget` and `MailboxByteBudget` are the caps on message
count and byte sum for the per-[owner](../../../../01-glossary.en.md#owner)
application mailbox. Byte accounting doesn't count only payload size —
it adds `payload size + metadata size + a fixed per-job cost`. Even if
payload is empty, one job isn't 0 bytes, and even for a large payload,
the fixed cost is still added. If the sum exceeds the `ulong`
representable range, it's pinned to `ulong.MaxValue` and that submit is
rejected. The accounting rule is owned by
[Framework API §8.2](../../../../06-framework-api.en.md#82-handler-execution-object-and-dependency-lifetime).
0 uses the Framework profile's finite default. Both values are set
before startup in `ConfigureRouterSocket()`, and Logical Multicast's
local target drop also follows this public capacity setting.

At runtime, `Mesh(meshName).PlacementWeight` and
`Channel(channelName).Weight` can be changed. The two weights are
independent of each other, and node weight is only used for object
create/relocation target selection. ChannelName uniquely selects a local
RouteMesh or ClientServer Server registration. HWM and timeout are set
before startup in `ConfigureRouterSocket()`.

`MaxMessageSize` is only set before startup, and a runtime setter isn't
provided. `0` uses the maximum complete message size the framework
supports. A positive value can't exceed the public protocol's
representation limit — exceeding it fails startup with
`ZLinkConfigurationException`. The smaller of the two endpoints'
allowed values applies per peer. The Framework application listener's
default is `16_777_216` bytes.

`ConfigureInboundDispatch()` returns one host-wide inbound setting.
`ApplicationHwmBytes`'s default is `null`, meaning Auto mode. `0` means
no limit, and a positive value is the exact byte cap.
`ApplicationHwmProfile`'s default is `Balanced`, and it's only used in
computation for Auto mode. `ProcessMemoryLimitBytes` only allows `null`
or a positive value. If this value is omitted in Auto mode, it checks the
finite OS cap applied to the process, such as a container/cgroup/Windows
Job Object, and the .NET GC managed heap cap
(`GC.GetGCMemoryInfo().TotalAvailableMemoryBytes`). If both are
confirmed, the smaller value is used; if only one is confirmed, that
value is used. If neither can be confirmed, the system's total physical
memory is used. So Auto mode boots even without configuration.

## 6. Messaging Metadata

The Node direct, ChannelName, Spot direct, Actor send/request, and
Logical Multicast call builders commonly have the following overload.
The handler context provides an immutable `ZLinkMessageMetadata`
snapshot.

```csharp
public interface IZLinkMetadataCall<TSelf>
{
    TSelf Metadata(string key, string value);
    TSelf Metadata(ZLinkMessageMetadata metadata);
}
```

Setting the same key multiple times sends the last value. The whole
metadata's UTF-8 encoded size can't exceed 1024 bytes. A reply doesn't
automatically copy request metadata, and a regular reply doesn't have a
metadata setter. The allowlist applied to STREAM session and Actor relay
is owned by the root's `ConfigureMetadata()`.

## 7. Location Store And Startup

A host using automatic discovery, distributed Spot/Actor address, or
Actor relocation must explicitly register a location store. The official
Redis location store package is the production default implementation.
Without registration, host startup fails. A process-local in-memory
implementation can only be registered in a single-process contract test.
The exact store capability and the Redis constructor/options are owned
by
[.NET Location And Maintenance](08-location-maintenance.en.md).
