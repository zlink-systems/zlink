# .NET RouteMesh/MeshNode Public Interface

[.NET per-language interface table of contents](README.en.md) · [Common Topology](../../../02-channel-transport/01-channel-topology.en.md) ·
[MeshNode](../../../03-spot-actor/03-mesh-node.en.md) · [Message Model](../../../00-foundation/05-message-model.en.md)

## 1. Scope

This document fixes ZLink Framework's .NET RouteMesh/MeshNode public
interface. The target audience is .NET application developers and public
provider implementers. This document owns the C# signature for
physical mesh registration, logical channel membership, manual peer,
handler, Spot/Actor registration, and runtime weight change.

## 2. Registration Interface

```csharp
public interface IZLinkFrameworkOptions
{
 TimeSpan DefaultRequestTimeout { get; set; }
 TimeSpan DefaultSocketSendTimeout { get; set; }
 TimeSpan SessionReplacementCallbackTimeout { get; set; }
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
 IZLinkNetworkOptions ConfigureNetwork();
 IZLinkDispatchOptions ConfigureDispatch();
 IZLinkInboundDispatchOptions ConfigureInboundDispatch();
 IZLinkStreamCompressionBuilder ConfigureStreamCompression();
 void UseFilter<TFilter>() where TFilter : class, IZLinkHandlerFilter;

 IZLinkMeshNodeBuilder AddRouteMesh(string meshName);
 IZLinkClientServerChannelRoleBuilder AddClientServerChannel(string channelName);
 IZLinkFanoutChannelBuilder AddFanoutChannel(string channelName);
 IZLinkStreamNodeBuilder AddStreamNode(string streamNodeName);
}

public enum ZLinkCoreHwmProfile
{
 Compact = 0,
 LowLatency = 1,
 Balanced = 2,
 Throughput = 3
}

public enum ZLinkApplicationJobQueueProfile
{
 Compact = 0,
 LowLatency = 1,
 Balanced = 2,
 Throughput = 3
}

public enum ZLinkApplicationJobQueuePressureState
{
 Running = 0,
 Paused = 1
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

public enum ZLinkSpotRelocationCoordinationMode
{
 FrameworkManaged = 0,
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
 IZLinkUserSpotFactoryBuilder<TSpot> RelocationCoordinationMode(
 ZLinkSpotRelocationCoordinationMode mode);
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
 IZLinkFanoutChannelBuilder SetNoDrop(bool noDrop = true);
 IZLinkFanoutChannelBuilder EnableSubscriber();
 IZLinkFanoutChannelBuilder Subscribe(string topic);
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
 IZLinkStreamNodeBuilder MaxMessageSize(long bytes);
 IZLinkStreamSocketConfig ConfigureSocket();
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

`IZLinkStreamNodeBuilder.MaxMessageSize(long bytes)` defaults to
`64 KiB`. It is used only when a StreamNode's Core STREAM inbound path checks
a complete client-to-server message, measured as header bytes plus payload
bytes and excluding the 6-byte prefix. `0` maps to Core `-1`, so Framework
adds no limit; a negative value is a startup configuration error. A message
over the limit is never partly delivered to the handler. The server records
`EMSGSIZE` and a diagnostic trace, then closes the connection. The raw client
observes the close rather than a separate wire error code. The Framework
limit doesn't apply to server-to-client outbound messages. ClientServer and
RouteMesh SS don't gain this setting.

The declaration of `IZLinkCodecRegistryBuilder` and the codec
extension is owned by [Serialization](11-serialization.en.md).

`AddRouteMesh(meshName)` registers one process-local
[MeshNode](../../../00-foundation/02-glossary.en.md#meshnode). Registering the same
`meshName` twice in the same process fails host startup with
`ZLinkConfigurationException`. After `Channel(channelName)`, call exactly
one of `Client()` or `Server()`. `Client()` only creates the send path,
and only `Server()` provides
[weight](../../../00-foundation/02-glossary.en.md#weight) and handler registration.
A MeshNode with no Server [membership](../../../00-foundation/02-glossary.en.md#membership)
can also start.

RouteMesh connection direction and peer necessity follow [Channel topology §8](../../../02-channel-transport/01-channel-topology.en.md).

[Channel topology §8](../../../02-channel-transport/01-channel-topology.en.md) defines manual handshake and NotRequired handling.

`Listen(string endpoint)`, `Bind(string endpoint)`, and
`EnablePublisher(string endpoint)` are provided, and the host/port
combination overload expresses the same listener configuration.

`AddClientServerChannel(channelName)` exposes `Client()` and `Server()`; registration and connection decisions follow [ClientServer channel](../../../02-channel-transport/03-client-server-channel.en.md).

[ClientServer channel §4](../../../02-channel-transport/03-client-server-channel.en.md) defines local Server candidacy and selection.

`ConfigureNetwork()`'s default BindHost is `127.0.0.1`, and if AdvertiseHost is omitted,
the default of [Network listener identity §2.1](../../../02-channel-transport/04-network-listener-identity.en.md#21-defaults) applies. An
[automatic discovery](../../../00-foundation/02-glossary.en.md#automatic-discovery)
listener binds to port `0` if the port on `Listen()`/`Bind()`/
`EnablePublisher()` is omitted, or if the listener call itself is
omitted. In manual mode, if the endpoint can't be obtained from a
different discovery source, the listen port and remote endpoint are
specified explicitly. A per-listener host setting takes priority over the
root default.

[Channel topology](../../../02-channel-transport/01-channel-topology.en.md) defines fanout descriptors, discovery, and connection direction; this builder exposes `EnableSubscriber()` and `Connect(endpoint)`.

Automatic RID has the format `prefix-<lowercase-canonical-uuid-v4>`.
UUID v4 is represented as a lowercase canonical string in `8-4-4-4-12`
digit groups. Prefix is ASCII `[A-Za-z0-9._-]` 1..64 characters, and the
full RID is at most 255 UTF-8 bytes. On conflict with an active owner, it
fails immediately with `RoutingIdConflict` instead of retrying with a
new UUID. [Common MeshNode §3.3](../../../03-spot-actor/03-mesh-node.en.md#33-fixed-rid) defines where a fixed RID can be used and how its restart conflicts are handled. Slot count, allocation
group, and a public allocation provider aren't provided.

The Object Server's Entry Spot ID also uses the same prefix, but with a
UUID v4 generated separately from the MeshNode RID attached. The format
is `<prefix>-entry-<lowercase-canonical-uuid-v4>`, and the caller doesn't
specify a fixed Entry Spot ID. This ID's global conflict and the reserved
format validation for a caller-specified Spot ID are defined by the
[Spot Model](../../../03-spot-actor/01-spot-model.en.md). The prefix and the
generated RID/Spot ID aren't interpreted as placement, shard, or stable
application identity.

A registered MeshNode descriptor must be at most 1 MiB. The
[Spot](../../../00-foundation/02-glossary.en.md#spot) type and stateful object
capability collection are each at most 1024. Exceeding the bound fails
startup — it doesn't apply only some of the registrations.

`SubscriberConnections` is a runtime handle for the manual subscriber
endpoint set. It provides connect, disconnect, and current-list query
targeting the same set of endpoints registered on the builder. An
automatic subscriber's discovery results aren't changed by this handle.

`AddHandlersFromAssemblyOf(...)` and `AddHandlersFromAssembly(...)` only
add the specified assembly to the handler scan scope. The
declaration of the method, group, and packet attributes used for the
scan is owned by [Common Runtime](01-common-runtime.en.md).

`EnableActorDispatch()` only activates a STREAM node's Actor dispatch
capability. If the same host has no Mesh whose object role is `Client`
or `Server`, and no Location Store, startup fails. Since the global
ActorId determines the current Mesh and owner route, this setting doesn't
take a MeshName.

`DefaultRequestTimeout`'s default is 30 seconds, and
`DefaultSocketSendTimeout`'s default is 1 second.
`SessionReplacementCallbackTimeout` is the maximum time an actor-binding replacement callback may
run before Framework force-closes the retired session; its default is 30 seconds. `Worker` sets the
worker's minimum/maximum thread count and idle timeout before host
startup.

`ConfigureStreamCompression()` and `IZLinkStreamCompressionBuilder` pick
the STREAM payload compression. This builder doesn't configure the
service transport lifecycle or a relocation codec.

`ApplicationVersion` is set once for the whole host, in the range
`0..long.MaxValue`, defaulting to `0`. Every local MeshNode publishes
this value, and a negative value is rejected with
`ZLinkConfigurationException` before startup. `MaintenanceWave` is a
stable ID that, when `null`, means no wave exclusion is used.

`Objects().Client()` and `Objects().Server()` are the .NET role builders; [MeshNode](../../../03-spot-actor/03-mesh-node.en.md) defines the roles.

[Channel topology](../../../02-channel-transport/01-channel-topology.en.md) defines Channel peer connection requirements for Object Client. Invalid Node direct handler registration maps to `ZLinkConfigurationException`.

[Channel topology](../../../02-channel-transport/01-channel-topology.en.md) defines Object Client pair connection requirements.

The Actor/User Spot/Instance Spot
[factory](../../../00-foundation/02-glossary.en.md#factory) fixes stable type,
per-object-kind factory options, and explicit relocation policy in the
same registration. There's no overload that omits the policy.
[Stable type](../../../00-foundation/02-glossary.en.md#stable-type) is UTF-8 1..255
bytes, and a duplicate type is a startup error. The Entry Spot ID is
issued by the framework.

The .NET placement builder exposes weight and population-capacity options; [Location runtime](../../../05-location-relocation/01-location-runtime.en.md) defines eligibility and capacity decisions.

`SetInstanceSpotIdleTimeout(...)` is the .NET option; [Spot model §6.2](../../../03-spot-actor/01-spot-model.en.md) defines its default, validation, and idle cleanup.

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

[Channel topology §8](../../../02-channel-transport/01-channel-topology.en.md) defines Connect peer admission and liveness.

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

`ChannelSend` and `ChannelRequest` cover RouteMesh and ClientServer. .NET maps filter failures to `ZLinkFrameworkErrorKind.InvalidOperation` and `Rejected`; [Framework API §10](../../../00-foundation/06-framework-api.en.md) defines filter execution and rejection.

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
target and adapter kind don't match, it fails with the same error. There
is no separate registration API.

The framework synchronously runs the callback exactly once inside the
registration call. Once the callback returns, the builder configuration
is fixed. If the application keeps the builder outside the callback and
calls it again, it's a configuration error. If the callback throws, the
factory isn't registered and the same exception is propagated to the
caller.

[Spot model](../../../03-spot-actor/01-spot-model.en.md) defines PerActor relocation policy; .NET names the option `ZLinkUserSpotExecutionMode.PerActor`.

[Spot model](../../../03-spot-actor/01-spot-model.en.md) defines execution and coordination mode defaults and eligibility; .NET exposes `SpotWide`, `FrameworkManaged`, and `ApplicationSignaled`.

If expected RID is omitted, the admission handshake determines the
remote identity. If expected RID is specified, the connection isn't
admitted if the handshake identity differs. A manual connection also uses
the same [MeshName](../../../00-foundation/02-glossary.en.md#meshname)/RID/
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
`ZLinkHandlerGroupAttribute` value. TicTacToe's manual topology does not mean
manual handler registration. The .NET sample exposes its handlers through
assembly scanning and `AddHandlerGroup(...)`; use typed
`AddSendHandler(...)`/`AddRequestHandler(...)` directly only for a separate
example that intentionally demonstrates direct registration.

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

public interface IZLinkStreamSocketConfig
{
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
 ulong SendHighWaterMark { get; set; }
 ulong ReceiveHighWaterMark { get; set; }
 TimeSpan? ReceiveTimeout { get; set; }
 TimeSpan? SendTimeout { get; set; }
}
```

`MaxMessageSize` is the .NET ClientServer application listener option. [Framework API §4](../../../00-foundation/06-framework-api.en.md#4-routemesh-registration) owns its default bound, the meaning of `0`, and its exclusion from RouteMesh.

`ConfigureSpotPublisher()` has no publish-only delivery policy option.
[Submit and completion §6](../../../01-execution/01-submit-and-completion.en.md)
defines Logical Multicast completion.

`IZLinkRouteMeshRuntimeOptions` is a public DI singleton. Querying
unregistered membership is `ZLinkConfigurationException`.

At runtime, `Mesh(meshName).PlacementWeight` and
`Channel(channelName).Weight` can be changed. The two weights are
independent of each other, and node weight is only used for object
create/relocation target selection. ChannelName uniquely selects a local
RouteMesh or ClientServer Server registration. HWM and timeout are set
before startup in `ConfigureRouterSocket()`.

`IZLinkMeshNodeSocketConfig` is the .NET socket-config surface; [Application job queue and backpressure](../../../01-execution/04-application-job-queue-and-backpressure.en.md) defines message bounds and resource guards.

`ConfigureInboundDispatch()` returns `IZLinkInboundDispatchOptions`; [Application job queue and backpressure](../../../01-execution/04-application-job-queue-and-backpressure.en.md) defines Core HWM, capacity, and threshold decisions.

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
The store capability and the Redis constructor/options are owned
by
[.NET Location And Maintenance](08-location-maintenance.en.md).
