# Java Configuration And Host Public Interface

[Interface table of contents](README.en.md) · [Transport Liveness](../../../../29-transport-liveness.en.md)

This document fixes the public interface a Java application uses to
configure Mesh, Channel role, handler, and the Framework host. The
application declares configuration with the builders below, and the
framework checks whether that configuration satisfies the contract when
starting the host.

```java
public interface ZLinkFrameworkOptions {
    Duration defaultRequestTimeout();
    void setDefaultRequestTimeout(Duration timeout);
    ZLinkCodecRegistryBuilder codecs();
    void addHandlersFromPackageOf(Class<?> markerType);
    ZLinkMetadataPolicyBuilder configureMetadata();
    void addLocationStore(ZLinkLocationStore store);
    void addRelocationStore(ZLinkRelocationStore store);
    void setApplicationVersion(long version);
    void setMaintenanceWave(String waveId);
    ZLinkLocationOptions configureLocations();
    ZLinkInboundDispatchOptions configureInboundDispatch();
    ZLinkNetworkOptions configureNetwork();
    ZLinkMeshNodeBuilder addRouteMesh(String meshName);
    ClientServerChannelBuilder addClientServerChannel(String channelName);
    FanoutChannelBuilder addFanoutChannel(String channelName);
    ZLinkStreamNodeBuilder addStreamNode(String name);
    void useFilter(Class<? extends ZLinkHandlerFilter> filterType);
    ZLinkDispatchOptions configureDispatch();
    ZLinkStreamCompressionBuilder configureStreamCompression();
    ZLinkWorkerOptions configureWorkers();
    void useVirtualThreadHandlers();
    void useHandlerExecutor(Executor executor);
}

public enum ZLinkApplicationHwmProfile {
    COMPACT,
    LOW_LATENCY,
    BALANCED,
    THROUGHPUT
}

public interface ZLinkInboundDispatchOptions {
    java.util.OptionalLong applicationHwmBytes();
    void setApplicationHwmBytes(long value);
    ZLinkApplicationHwmProfile applicationHwmProfile();
    void setApplicationHwmProfile(ZLinkApplicationHwmProfile value);
    java.util.OptionalLong processMemoryLimitBytes();
    void setProcessMemoryLimitBytes(long value);
}

public interface ZLinkLocationOptions {
    Duration ownerLeaseRenewInterval();
    void setOwnerLeaseRenewInterval(Duration value);
    Duration ownerLeaseTtl();
    void setOwnerLeaseTtl(Duration value);
    Duration pollingInterval();
    void setPollingInterval(Duration value);
    Duration storeFailureGrace();
    void setStoreFailureGrace(Duration value);
    Duration ownerLeaseFencingMargin();
    void setOwnerLeaseFencingMargin(Duration value);
    Duration ownerLeaseRenewTimeout();
    void setOwnerLeaseRenewTimeout(Duration value);
    Duration routeCacheMaxAge();
    void setRouteCacheMaxAge(Duration value);
    Duration messageFollowDuration();
    void setMessageFollowDuration(Duration value);
    int maxActiveOutboundRelocations();
    void setMaxActiveOutboundRelocations(int value);
    int maxActiveInboundRelocations();
    void setMaxActiveInboundRelocations(int value);
    int maxConcurrentRelocationCaptures();
    void setMaxConcurrentRelocationCaptures(int value);
    int maxConcurrentRelocationRestores();
    void setMaxConcurrentRelocationRestores(int value);
    long maxRelocationPayloadInFlightBytes();
    void setMaxRelocationPayloadInFlightBytes(long value);
}

public interface ZLinkNetworkOptions {
    String bindHost();
    void setBindHost(String host);
    Optional<String> advertiseHost();
    void setAdvertiseHost(String host);
}

public interface ZLinkMeshNodeSocketConfig {
    long maxMessageSize();
    void setMaxMessageSize(long value);
    long sendHighWaterMark();
    void setSendHighWaterMark(long value);
    long receiveHighWaterMark();
    void setReceiveHighWaterMark(long value);
    long mailboxMessageBudget();
    void setMailboxMessageBudget(long value);
    long mailboxByteBudget();
    void setMailboxByteBudget(long value);
    Optional<Duration> receiveTimeout();
    void setReceiveTimeout(Duration value);
    Optional<Duration> sendTimeout();
    void setSendTimeout(Duration value);
}

@FunctionalInterface
public interface ZLinkFrameworkConfigurer {
    void configure(ZLinkFrameworkOptions framework);
}

public interface ZLinkMeshNodeBuilder {
    ZLinkMeshChannelBuilder channel(String channelName);
    ZLinkMeshNodeBuilder listen(String endpoint);
    ZLinkMeshNodeBuilder listen();
    ZLinkMeshNodeBuilder listen(int port);
    ZLinkMeshNodeBuilder setBindHost(String host);
    ZLinkMeshNodeBuilder setAdvertiseHost(String host);
    ZLinkMeshNodeBuilder setRoutingId(RoutingId routingId);
    ZLinkMeshNodeBuilder setRoutingIdPrefix(String prefix);
    ZLinkMeshNodeBuilder setPlacementWeight(int weight);
    ZLinkMeshNodeBuilder setActorCapacity(int maxActors);
    ZLinkMeshNodeBuilder setSpotCapacity(int maxSpots);
    ZLinkMeshNodeBuilder setActivationConcurrency(int maxConcurrentActivations);
    ZLinkMeshNodeBuilder setInstanceSpotIdleTimeout(Duration timeout);
    ZLinkMeshObjectRoleBuilder objects();
    ZLinkMeshNodeSocketConfig configureRouterSocket();
    ZLinkSpotPublisherConfig configureSpotPublisher();
    ZLinkMeshPeerConnections peerConnections();
    ZLinkMeshNodeBuilder setDefaultRequestTimeout(Duration timeout);

    <THandler, TMessage>
    ZLinkMeshNodeBuilder addRouteSendHandler(
        Class<THandler> handlerType,
        Class<TMessage> messageType);
    <THandler, TRequest, TReply>
    ZLinkMeshNodeBuilder addRouteRequestHandler(
        Class<THandler> handlerType,
        Class<TRequest> requestType,
        Class<TReply> replyType);

}

public interface ZLinkMeshObjectRoleBuilder {
    ZLinkMeshObjectClientBuilder client();
    ZLinkMeshObjectServerBuilder server();
}

public interface ZLinkMeshObjectClientBuilder {}

public interface ZLinkMeshObjectServerBuilder {
    ZLinkMeshObjectServerBuilder addEntrySpot(Class<? extends ZLinkEntrySpot> entrySpotClass);
    <TSpot extends ZLinkSpot> ZLinkMeshObjectServerBuilder addSpotFactory(
        String spotType, Class<TSpot> spotClass,
        Consumer<ZLinkUserSpotFactoryBuilder<TSpot>> configure);
    <TSpot extends ZLinkInstanceSpot> ZLinkMeshObjectServerBuilder addInstanceSpotFactory(
        String instanceSpotType, Class<TSpot> spotClass,
        Consumer<ZLinkInstanceSpotFactoryBuilder<TSpot>> configure);
    <TActor extends ZLinkActor> ZLinkMeshObjectServerBuilder addActorFactory(
        String actorType,
        Class<TActor> actorClass,
        Class<? extends ZLinkActorFactory> factoryClass,
        Consumer<ZLinkActorFactoryBuilder<TActor>> configure);
}

public enum ZLinkUserSpotExecutionMode {
    SPOT_WIDE(0), PER_ACTOR(1);
    private final int value;
    ZLinkUserSpotExecutionMode(int value) { this.value = value; }
    public int value() { return value; }
}

public enum ZLinkSpotRelocationReadinessMode {
    ANY_TURN_BOUNDARY(0), APPLICATION_SIGNALED(1);
    private final int value;
    ZLinkSpotRelocationReadinessMode(int value) { this.value = value; }
    public int value() { return value; }
}

public interface ZLinkActorFactoryBuilder<TActor extends ZLinkActor> {
    void disableRelocation();
    void recreateOnRelocation();
    void preserveStateWith(
        Class<? extends ZLinkActorRelocationAdapter<TActor>> adapterClass);
}

public interface ZLinkUserSpotFactoryBuilder<TSpot extends ZLinkSpot> {
    ZLinkUserSpotFactoryBuilder<TSpot> stableTypeLimit(int limit);
    ZLinkUserSpotFactoryBuilder<TSpot> executionMode(ZLinkUserSpotExecutionMode mode);
    ZLinkUserSpotFactoryBuilder<TSpot> relocationReadiness(
        ZLinkSpotRelocationReadinessMode mode);
    void disableRelocation();
    void recreateOnRelocation();
    void preserveStateWith(
        Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass);
}

public interface ZLinkInstanceSpotFactoryBuilder<TSpot extends ZLinkInstanceSpot> {
    ZLinkInstanceSpotFactoryBuilder<TSpot> stableTypeLimit(int limit);
    void disableRelocation();
    void recreateOnRelocation();
    void preserveStateWith(
        Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass);
}

public interface FanoutChannelBuilder {
    FanoutChannelBuilder enablePublisher(String endpoint);
    FanoutChannelBuilder enablePublisher();
    FanoutChannelBuilder enablePublisher(int port);
    FanoutChannelBuilder setBindHost(String host);
    FanoutChannelBuilder setAdvertiseHost(String host);
    FanoutChannelBuilder setRoutingId(RoutingId publisherRoutingId);
    FanoutChannelBuilder setRoutingIdPrefix(String prefix);
    FanoutChannelBuilder enableSubscriber();
    FanoutChannelBuilder connect(String endpoint);
    ZLinkEndpointConnections subscriberConnections();
    FanoutChannelBuilder addHandlerGroup(String groupName);
}
```

If `applicationHwmBytes()` is empty, it's Auto mode. The setter's `0`
means no limit, a positive value is the exact host-wide byte cap, and a
negative value is a startup configuration error. The profile default is
`BALANCED`. If `processMemoryLimitBytes()` is empty, it checks the
finite OS cap applied to the process, such as a container/cgroup/Windows
Job Object, and the JVM managed heap cap (`Runtime.maxMemory()`). If
both are confirmed, the smaller value is used; if only one is confirmed,
that value is used. If neither can be confirmed, the system's total
physical memory is used. Auto mode boots even without configuration, and
startup only fails before socket bind when the computed result isn't
positive. The application listener's default `maxMessageSize()` is
`16_777_216L` bytes.

`configureNetwork()` returns the default host the process's RouteMesh,
ClientServer, classic fanout, and stream listener use. The default
BindHost is `127.0.0.1`. Calling per-listener `setBindHost(...)` and
`setAdvertiseHost(...)` overrides the root default only for that
listener. When using port `0`, the framework combines the port
confirmed after bind with AdvertiseHost and records it in the discovery
descriptor. Since a stream listener isn't a discovery target, it uses
the advertised endpoint computed with the same rule for operational
information, and doesn't automatically publish the remote connector
endpoint.

The following example is a minimal form registering, on different host
configurations, a node that only starts calls and a node that processes
requests on the same RouteMesh. `clientOptions` and `serverOptions` are
each a separate host's `ZLinkFrameworkOptions`. The names and weight in
the example are illustrative values, not contract defaults.

```java
clientOptions.addRouteMesh("orders")
    .channel("checkout")
    .client(); // this node starts checkout calls but isn't included as a server candidate.

serverOptions.addRouteMesh("orders")
    .channel("checkout")
    .server()
    .setWeight(100)
    .addRequestHandler(
        CheckoutHandler.class,
        CheckoutRequest.class,
        CheckoutReply.class); // registers this node as a checkout request processing candidate.
```

An automatic [RouteMesh](../../../../01-glossary.en.md#routemesh)
compares RID in canonical byte order, and only the MeshNode with the
smaller RID connects to the counterpart endpoint. A manual topology can
connect from one or both sides depending on application endpoint
configuration. If bidirectional connection or automatic discovery
contention/a stale snapshot creates a duplicate candidate, handshake and
admission check the same RID and lifecycle generation and keep only one
in ready state.

A peer connection isn't needed only when both MeshNodes' object role is
`Client` and neither has RouteMesh Channel Server membership. The same
applies when only Channel Client membership is registered. If either
side has Channel Server membership, a connection is made and liveness is
kept even if weight is `0`. ClientServer and classic fanout
registration are separate physical topologies, so they aren't included
in this judgment.

ClientServer can use manual endpoint and location store
[automatic discovery](../../../../01-glossary.en.md#automatic-discovery)
together. If the two sources point to the same Server RID and
[lifecycle generation](../../../../01-glossary.en.md#lifecycle-generation),
the connection intent and ready target are merged into one. In both
automatic and manual, only Client connects to server — Server doesn't
look for a client endpoint or start an outbound connect. Client and
Server can each be registered once on the same ChannelName, sharing one
ClientServer topology through separate registrations under the
`(ChannelName, Role)` key. Registering the same role twice fails
startup, and the RouteMesh [ChannelName](../../../../01-glossary.en.md#channelname)
conflict rule is kept. A local Server, after listener and service
admission, is also selected under the same readiness/[weight](../../../../01-glossary.en.md#weight)/
drain conditions as a remote Server, without local priority or calling a
direct handler.

In fanout, the Publisher only publishes a descriptor and doesn't start
an outbound connect. Only the subscriber connects to the publisher
endpoint, and an automatic subscriber creates one connection intent per
Publisher RID and lifecycle generation. Configuring both an automatic
subscriber and a manual subscriber endpoint on one ChannelName fails
startup.

Omitting object role means `None`. `client()` only provides global
object operations and doesn't become a placement target, and `server()`
provides Client capability plus Entry Spot/factory registration. Client
and Server require a
[Location Store](../../../../01-glossary.en.md#location-store). The
Actor/User Spot/Instance Spot
[factory](../../../../01-glossary.en.md#factory) must take a stable type
and configure callback, and there's no overload that omits relocation
behavior selection.

A RouteMesh Channel Server can also be registered on an Object Client. An
application Node direct handler can't be registered, and specifying an
Object Client RID as a Node direct target ends as not-found without
switching to a different RID.

The configure callback must call exactly one of `disableRelocation()`,
`recreateOnRelocation()`, `preserveStateWith(...)`. Omitting it or
calling more than one is a startup configuration error before socket
bind. The Actor builder only takes a `ZLinkActorRelocationAdapter` of
the same Actor type, and the User/Instance Spot builder only takes a
`ZLinkSpotRelocationAdapter` of the same Spot type.

The framework runs the configure callback synchronously exactly once
inside the registration call. Calling the retained builder again after
the callback returns is a configuration error. If the callback throws,
that factory isn't registered and the same exception is propagated to
the caller.

`ZLinkUserSpotExecutionMode.PER_ACTOR` only allows
`recreateOnRelocation()`. Registering a different policy together is a
startup configuration error. A PerActor Spot is a stateless execution
shell, and the Actor policy and adapter each handle Actor state.

`relocationReadiness`'s default is `ANY_TURN_BOUNDARY`.
`APPLICATION_SIGNALED` is only allowed with `SPOT_WIDE`, and registering
it together with `PER_ACTOR` is a startup configuration error before
socket bind. Since the Spot callback is a default no-op method, an
application override isn't required.

Node placement weight is 0..10000, defaulting to 100. An out-of-range
value is a configuration error in both startup config and runtime
change. Node capacity defaults to 10,000 active and 128 pending. If a
per-type limit is omitted, it shares the node limit. An explicit value
must be 1..`Integer.MAX_VALUE`, and a value smaller than the node limit
applies. `stableTypeLimit(0)` and a negative value are a configuration
error during callback execution. The capacity filter is applied before
weight. `enableActorDispatch()` takes no argument, and the global
ActorId resolves the Mesh.

The Object Server's Entry Spot ID has the format
`<prefix>-entry-<lowercase-canonical-uuid-v4>` using the MeshNode
diagnostic prefix, with a UUID v4 generated separately from the
MeshNode. The framework-internal descriptor's `entrySpotId` provides the
exact mapping for the same lifecycle. If the global Spot ID conflicts
with an active owner, startup fails immediately with a configuration
exception instead of retrying with a new UUID. If a caller-specified
User/Instance Spot ID matches this reserved format, it's rejected with
`INVALID_OPERATION` before starting the Store and factory.

The Location provider provides, through `ZLinkLocationStore`, read of
the framework's opaque record, version-conditional atomic batch, and
bounded snapshot scan. A separate per-domain Store instance isn't
registered on the host. A host that selected `recreateOnRelocation()` or
`preserveStateWith(...)` even once, or registered even one Instance Spot
factory, registers exactly one `ZLinkRelocationStore`. A host with no
Instance Spot factory, where every factory selected
`disableRelocation()` and only uses same-node joins, can omit the
Relocation Store. A missing or duplicate Store registration is a startup
configuration error before socket bind. An API that registers Location
and Relocation capability together, and a Redis-specific registration
helper, aren't provided.

`ApplicationVersion` is a deployment sequence number in range
`0..Long.MAX_VALUE`. A negative value is rejected in startup validation.
The 5-second periodic probe independent of application traffic, and the
15-second matching-ACK deadline on the same current connection, are the
JVM service runtime's fixed liveness profile. A different inbound frame
doesn't satisfy the [deadline](../../../../01-glossary.en.md#deadline).
This value isn't exposed as a public option per Channel/handler/peer.
The Location owner lease option is a separate store contract and doesn't
substitute for transport liveness.

`maxMessageSize` is only set before startup, and a runtime setter isn't
provided. `0` normalizes to the maximum complete message size the
bindings or transport can receive. If transport is unlimited, it uses
the service wire's `uint32` representation limit minus envelope
overhead. A positive value can't exceed that representation limit —
exceeding it is rejected as a startup configuration error. Peers exchange
the normalized value in the internal handshake, and sender and receiver
each apply the smaller of the two values as the effective bound before
complete message allocation. A public option for this negotiation isn't
provided.

`mailboxMessageBudget` and `mailboxByteBudget` are the caps on message
count and byte sum for the per-owner application mailbox. Byte
accounting doesn't count only payload size — it adds `payload size +
metadata size + a fixed per-job cost`. Even if payload is empty, one job
isn't `0` bytes, and even for a large payload, the fixed cost is still
added. If the sum exceeds the `long` representable range, it's pinned to
`Long.MAX_VALUE` and that submit is rejected. The accounting rule is
owned by
[Framework API §8.2](../../../../06-framework-api.en.md#82-handler-execution-object-and-dependency-lifetime).
Both values are only set before startup. `0` isn't unlimited — it
selects the Framework profile's finite default. A negative value is a
startup configuration error. A Logical Multicast local target also
judges admission using this capacity limit.

`setInstanceSpotIdleTimeout(...)` is the reference time for cleaning up
an idle Instance Spot. The default is `Duration.ZERO`, and
`Duration.ZERO` means no cleanup. The allowed range is `Duration.ZERO`
and positive values — `null` and a negative value are startup
configuration errors. The value is fixed before the MeshNode lifecycle
starts, and a runtime setter isn't provided. It's a separate setting
from `ZLinkWorkerOptions.idleTimeout(...)`, and they don't inherit each
other's value. Only Instance Spot is a cleanup target — Entry Spot and
User Spot aren't affected by this setting. The idle judgment condition,
the delivery of `ZLinkSpotCloseReason.IDLE_EVICTED`, and the
cold-activation rule after cleanup are owned by
[Spot Model §6.2](../../../../11-spot-model.en.md#62-cleaning-up-an-idle-instance-spot).

Automatic RID has the format `prefix-<lowercase-canonical-uuid-v4>`.
UUID v4 is represented as a lowercase canonical string in `8-4-4-4-12`
digit groups. Prefix is ASCII `[A-Za-z0-9._-]` 1..64 characters, and on
conflict with an active [owner](../../../../01-glossary.en.md#owner), it
fails immediately with `ROUTING_ID_CONFLICT` instead of retrying with a
new UUID. Fixed RID is only allowed in a manual topology with no object
role and no Store descriptor. Slot count, allocation group, and a public
allocation provider aren't provided.

The fully encoded MeshNode descriptor the framework builds from every
registration must be at most 1 MiB.
[Spot](../../../../01-glossary.en.md#spot) type and object capability
collection are each at most 1024. The relocation adapter class and
opaque application bytes aren't published in the peer descriptor. The
runtime validates the completed descriptor all at once before socket
bind. Exceeding the bound fails startup — it doesn't truncate/split the
collection or publish part of the descriptor.

## Exact Public Member `javap` Inventory

The declarations below fix this category's Java public types and
members in the binary signature format `javap` prints.

```java
public interface systems.zlink.framework.spring.ZLinkFrameworkConfigurer {
  public abstract void configure(systems.zlink.framework.configuration.ZLinkFrameworkOptions);
}
public interface systems.zlink.framework.configuration.ZLinkNetworkOptions {
  public abstract java.lang.String bindHost();
  public abstract void setBindHost(java.lang.String);
  public abstract java.util.Optional<java.lang.String> advertiseHost();
  public abstract void setAdvertiseHost(java.lang.String);
}
public interface systems.zlink.framework.configuration.FanoutChannelBuilder {
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder enablePublisher(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder enablePublisher();
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder enablePublisher(int);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder setBindHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder setAdvertiseHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder setRoutingId(systems.zlink.contracts.core.RoutingId);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder setRoutingIdPrefix(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder enableSubscriber();
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder connect(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkEndpointConnections subscriberConnections();
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder addHandlerGroup(java.lang.String);
  public abstract void addPublishHandler(java.lang.Class<?>, java.lang.Class<?>);
  public abstract void addPublishHandler(java.lang.Class<?>, java.lang.Class<?>, java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder addPublishHandler(java.lang.Class<?>);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder addPublishHandler(java.lang.Class<?>, java.lang.String);
}
public interface systems.zlink.framework.configuration.ZLinkCodecExtension {
  public abstract void register(systems.zlink.framework.configuration.ZLinkCodecRegistrar);
}
public interface systems.zlink.framework.configuration.ZLinkCodecRegistrar {
  public abstract void addSerializer(java.lang.String, systems.zlink.framework.ZLinkMessageSerializer);
  public abstract void addSerializer(java.lang.String, systems.zlink.framework.ZLinkMessageSerializer, java.util.function.Predicate<java.lang.Class<?>>);
  public abstract void addStreamCodec(java.lang.String, systems.zlink.framework.streams.ZLinkStreamCodec);
}
public interface systems.zlink.framework.configuration.ZLinkCodecRegistryBuilder {
  public abstract void use(systems.zlink.framework.configuration.ZLinkCodecExtension);
}
public interface systems.zlink.framework.configuration.ZLinkDiagnosticsOptions {
  public abstract systems.zlink.framework.configuration.ZLinkMessageFlowLogMode messageFlow();
  public abstract double sampleRate();
  public abstract boolean includeMessageSizes();
  public abstract boolean includeNativeDiagnostics();
  public abstract java.lang.String logFile();
  public abstract java.lang.String label();
  public abstract systems.zlink.framework.configuration.ZLinkMessageFlowLogMode effectiveMessageFlow();
}
public interface systems.zlink.framework.configuration.ZLinkDispatchOptions {
  public abstract systems.zlink.framework.configuration.ZLinkUnhandledDispatchOptions unhandled();
  public abstract systems.zlink.framework.configuration.ZLinkDiagnosticsOptions diagnostics();
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions setMessageFlowObserver(java.lang.Class<? extends systems.zlink.framework.configuration.ZLinkMessageFlowObserver>);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions setMessageFlowObserver(systems.zlink.framework.configuration.ZLinkMessageFlowObserver);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions traceSampleRate(double);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions includeMessageSizes(boolean);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions traceLogFile(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions traceLabel(java.lang.String);
}
public interface systems.zlink.framework.configuration.ZLinkEndpointConnections {
  public abstract void connect(java.lang.String);
  public abstract void disconnect(java.lang.String);
  public abstract java.util.List<java.lang.String> listConnections();
}
public interface systems.zlink.framework.configuration.ZLinkMeshNodeSocketConfig {
  public abstract long maxMessageSize();
  public abstract void setMaxMessageSize(long);
  public abstract long sendHighWaterMark();
  public abstract void setSendHighWaterMark(long);
  public abstract long receiveHighWaterMark();
  public abstract void setReceiveHighWaterMark(long);
  public abstract long mailboxMessageBudget();
  public abstract void setMailboxMessageBudget(long);
  public abstract long mailboxByteBudget();
  public abstract void setMailboxByteBudget(long);
  public abstract java.util.Optional<java.time.Duration> receiveTimeout();
  public abstract void setReceiveTimeout(java.time.Duration);
  public abstract java.util.Optional<java.time.Duration> sendTimeout();
  public abstract void setSendTimeout(java.time.Duration);
}
public final class systems.zlink.framework.configuration.ZLinkMeshPeerConnection extends java.lang.Record {
  public systems.zlink.framework.configuration.ZLinkMeshPeerConnection(java.lang.String, java.util.Optional<systems.zlink.contracts.core.RoutingId>);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public java.lang.String endpoint();
  public java.util.Optional<systems.zlink.contracts.core.RoutingId> expectedRoutingId();
}
public interface systems.zlink.framework.configuration.ZLinkMeshPeerConnections {
  public abstract void connect(java.lang.String);
  public abstract void connect(systems.zlink.contracts.core.RoutingId, java.lang.String);
  public abstract void disconnect(java.lang.String);
  public abstract java.util.List<systems.zlink.framework.configuration.ZLinkMeshPeerConnection> listConnections();
}
public interface systems.zlink.framework.configuration.ZLinkMessageFlowControl {
  public abstract void setMessageFlowMode(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode);
  public abstract systems.zlink.framework.configuration.ZLinkMessageFlowLogMode messageFlowMode();
}
public final class systems.zlink.framework.configuration.ZLinkMessageFlowEvent extends java.lang.Record {
  public systems.zlink.framework.configuration.ZLinkMessageFlowEvent(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome, systems.zlink.framework.configuration.ZLinkDispatchErrorSurface, systems.zlink.framework.configuration.ZLinkDispatchMessageKind, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.Long, systems.zlink.framework.configuration.ZLinkDispatchErrorReason, systems.zlink.framework.configuration.ZLinkDispatchErrorAction, java.lang.String, java.lang.String, java.lang.String, systems.zlink.framework.monitoring.ZLinkFlowOrigin);
  public systems.zlink.framework.configuration.ZLinkMessageFlowEvent(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome, systems.zlink.framework.configuration.ZLinkDispatchErrorSurface, systems.zlink.framework.configuration.ZLinkDispatchMessageKind, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.Long, systems.zlink.framework.configuration.ZLinkDispatchErrorReason, systems.zlink.framework.configuration.ZLinkDispatchErrorAction, java.lang.String, java.lang.String);
  public systems.zlink.framework.configuration.ZLinkMessageFlowEvent(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome, systems.zlink.framework.configuration.ZLinkDispatchErrorSurface, systems.zlink.framework.configuration.ZLinkDispatchMessageKind, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.Long);
  public systems.zlink.framework.configuration.ZLinkMessageFlowEvent withFlow(java.lang.String, systems.zlink.framework.monitoring.ZLinkFlowOrigin);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.configuration.ZLinkMessageFlowOutcome outcome();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorSurface surface();
  public systems.zlink.framework.configuration.ZLinkDispatchMessageKind messageKind();
  public java.lang.String packetName();
  public java.lang.String channelName();
  public java.lang.String topic();
  public java.lang.String correlationId();
  public java.lang.String sourceRid();
  public java.lang.String spotId();
  public java.lang.String actorId();
  public java.lang.Long messageSize();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorReason errorReason();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorAction errorAction();
  public java.lang.String errorType();
  public java.lang.String errorMessage();
  public java.lang.String flowId();
  public systems.zlink.framework.monitoring.ZLinkFlowOrigin flowOrigin();
}
public final class systems.zlink.framework.configuration.ZLinkMessageFlowLogMode extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkMessageFlowLogMode> {
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode OFF;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode ERRORS_ONLY;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode KEY_TRANSITIONS;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode VERBOSE;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode DIAGNOSTIC;
  public static systems.zlink.framework.configuration.ZLinkMessageFlowLogMode[] values();
  public static systems.zlink.framework.configuration.ZLinkMessageFlowLogMode valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkMessageFlowObserver {
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> onMessageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowEvent);
}
public interface systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder allowSessionToActor(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder allowActorToSession(java.lang.String);
}
public interface systems.zlink.framework.configuration.ZLinkSpotPublisherConfig {
  public abstract long sendHighWaterMark();
  public abstract void setSendHighWaterMark(long);
  public abstract java.util.Optional<java.time.Duration> sendTimeout();
  public abstract void setSendTimeout(java.time.Duration);
  public abstract java.util.Optional<java.time.Duration> linger();
  public abstract void setLinger(java.time.Duration);
}
public interface systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder useDefault();
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder useLz4();
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder use(systems.zlink.framework.streams.ZLinkStreamCompressionCodec);
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder disable();
}
public interface systems.zlink.framework.configuration.ZLinkStreamNodeBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder bind(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder bind();
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder bind(int);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder setBindHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder setAdvertiseHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamSocketConfig configureSocket();
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder setTlsServer(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder setTlsServer(java.lang.String, java.lang.String, boolean);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder registerSession(java.lang.Class<? extends systems.zlink.framework.streams.ZLinkSession>);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder enableActorDispatch();
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder addSessionPacketHandler(java.lang.Class<?>);
}
public interface systems.zlink.framework.configuration.ZLinkStreamSocketConfig {
  public abstract long maxMessageSize();
  public abstract void setMaxMessageSize(long);
}
public final class systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction> {
  public static final systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction REPLY_ERROR;
  public static final systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction LOG_AND_DROP;
  public static final systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction DROP;
  public static final systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction THROW;
  public static systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction[] values();
  public static systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkUnhandledDispatchOptions {
  public abstract void setRequest(systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction);
  public abstract void setSend(systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction);
  public abstract void setPublish(systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction);
  public abstract void setSendLogLevel(systems.zlink.framework.configuration.ZLinkLogLevel);
  public abstract void setPublishLogLevel(systems.zlink.framework.configuration.ZLinkLogLevel);
}
public interface systems.zlink.framework.configuration.ZLinkWorkerOptions {
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions minThreads(int);
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions maxThreads(int);
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions idleTimeout(java.time.Duration);
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions maxQueueLength(int);
}
```

`ZLinkStreamNodeBuilder.configureSocket().setMaxMessageSize(...)` defaults
to `64 KiB`. This setting is used only when a StreamNode's Core STREAM
inbound path checks a complete client-to-server message. The size is header
bytes plus payload bytes, excluding the 6-byte prefix. `0` maps to Core `-1`,
so Framework adds no limit; a negative value is a startup configuration
error. A message over the limit is never partly delivered to the handler. The
server records `EMSGSIZE` and a diagnostic trace, then closes the connection.
The raw client observes the close without a separate wire error code. The
Framework limit doesn't apply to server-to-client outbound messages, and the
setting isn't added to ClientServer or RouteMesh SS.

## Remaining Configuration Public Member `javap` Inventory

The declarations below fix, in the binary signature format `javap`
prints, the application-facing configuration types not included in the
inventory above.

```java
public final class systems.zlink.framework.configuration.ZLinkDispatchErrorAction extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkDispatchErrorAction> {
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorAction REPLY_ERROR;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorAction DROP;
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorAction[] values();
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorAction valueOf(java.lang.String);
  public int value();
}
public final class systems.zlink.framework.configuration.ZLinkDispatchErrorReason extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkDispatchErrorReason> {
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason HANDLER_MISSING;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason PAYLOAD_DECODE_FAILED;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason HANDLER_EXCEPTION;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason INVALID_FRAME;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason REPLY_PATH_MISSING;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason UNEXPECTED_REPLY;
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorReason[] values();
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorReason valueOf(java.lang.String);
  public int value();
}
public final class systems.zlink.framework.configuration.ZLinkDispatchErrorSurface extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkDispatchErrorSurface> {
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface CHANNEL;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface ROUTE_MESH_CHANNEL;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface SPOT_ROUTE;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface SPOT_SUBSCRIPTION;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface SPOT_ACTOR;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface STREAM_SESSION;
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorSurface[] values();
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorSurface valueOf(java.lang.String);
  public int value();
}
public final class systems.zlink.framework.configuration.ZLinkDispatchFailure extends java.lang.Record {
  public systems.zlink.framework.configuration.ZLinkDispatchFailure(systems.zlink.framework.configuration.ZLinkDispatchErrorSurface, systems.zlink.framework.configuration.ZLinkDispatchMessageKind, systems.zlink.framework.configuration.ZLinkDispatchErrorReason, systems.zlink.framework.configuration.ZLinkDispatchErrorAction, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.configuration.ZLinkDispatchErrorSurface surface();
  public systems.zlink.framework.configuration.ZLinkDispatchMessageKind messageKind();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorReason reason();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorAction action();
  public java.lang.String packetName();
  public java.lang.String channelName();
  public java.lang.String topic();
  public java.lang.String spotId();
  public java.lang.String actorId();
  public java.lang.String sourceRid();
  public java.lang.String correlationId();
  public java.lang.String errorType();
  public java.lang.String errorMessage();
}
public final class systems.zlink.framework.configuration.ZLinkDispatchMessageKind extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkDispatchMessageKind> {
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind REQUEST;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind SEND;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind PUBLISH;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind RESPONSE;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind ERROR;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind ACTOR_REQUEST;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind ACTOR_SEND;
  public static systems.zlink.framework.configuration.ZLinkDispatchMessageKind[] values();
  public static systems.zlink.framework.configuration.ZLinkDispatchMessageKind valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkFrameworkOptions {
  public abstract java.time.Duration defaultRequestTimeout();
  public abstract void setDefaultRequestTimeout(java.time.Duration);
  public abstract systems.zlink.framework.configuration.ZLinkCodecRegistryBuilder codecs();
  public abstract void addHandlersFromPackageOf(java.lang.Class<?>);
  public abstract systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder configureMetadata();
  public abstract void addRelocationStore(systems.zlink.framework.locations.ZLinkRelocationStore);
  public abstract void setApplicationVersion(long);
  public abstract void setMaintenanceWave(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder addRouteMesh(java.lang.String);
  public abstract systems.zlink.framework.configuration.ClientServerChannelBuilder addClientServerChannel(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder addFanoutChannel(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder addStreamNode(java.lang.String);
  public abstract void addLocationStore(systems.zlink.framework.locations.ZLinkLocationStore);
  public abstract systems.zlink.framework.locations.ZLinkLocationOptions configureLocations();
  public abstract systems.zlink.framework.configuration.ZLinkInboundDispatchOptions configureInboundDispatch();
  public abstract systems.zlink.framework.configuration.ZLinkNetworkOptions configureNetwork();
  public abstract void useFilter(java.lang.Class<? extends systems.zlink.framework.ZLinkHandlerFilter>);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions configureDispatch();
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder configureStreamCompression();
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions configureWorkers();
  public abstract void useVirtualThreadHandlers();
  public abstract void useHandlerExecutor(java.util.concurrent.Executor);
}
public interface systems.zlink.framework.locations.ZLinkLocationOptions {
  public abstract java.time.Duration ownerLeaseRenewInterval();
  public abstract void setOwnerLeaseRenewInterval(java.time.Duration);
  public abstract java.time.Duration ownerLeaseTtl();
  public abstract void setOwnerLeaseTtl(java.time.Duration);
  public abstract java.time.Duration pollingInterval();
  public abstract void setPollingInterval(java.time.Duration);
  public abstract java.time.Duration storeFailureGrace();
  public abstract void setStoreFailureGrace(java.time.Duration);
  public abstract java.time.Duration ownerLeaseFencingMargin();
  public abstract void setOwnerLeaseFencingMargin(java.time.Duration);
  public abstract java.time.Duration ownerLeaseRenewTimeout();
  public abstract void setOwnerLeaseRenewTimeout(java.time.Duration);
  public abstract java.time.Duration routeCacheMaxAge();
  public abstract void setRouteCacheMaxAge(java.time.Duration);
  public abstract java.time.Duration messageFollowDuration();
  public abstract void setMessageFollowDuration(java.time.Duration);
  public abstract int maxActiveOutboundRelocations();
  public abstract void setMaxActiveOutboundRelocations(int);
  public abstract int maxActiveInboundRelocations();
  public abstract void setMaxActiveInboundRelocations(int);
  public abstract int maxConcurrentRelocationCaptures();
  public abstract void setMaxConcurrentRelocationCaptures(int);
  public abstract int maxConcurrentRelocationRestores();
  public abstract void setMaxConcurrentRelocationRestores(int);
  public abstract long maxRelocationPayloadInFlightBytes();
  public abstract void setMaxRelocationPayloadInFlightBytes(long);
}
public final class systems.zlink.framework.configuration.ZLinkLogLevel extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkLogLevel> {
  public static final systems.zlink.framework.configuration.ZLinkLogLevel TRACE;
  public static final systems.zlink.framework.configuration.ZLinkLogLevel DEBUG;
  public static final systems.zlink.framework.configuration.ZLinkLogLevel INFO;
  public static final systems.zlink.framework.configuration.ZLinkLogLevel WARN;
  public static final systems.zlink.framework.configuration.ZLinkLogLevel ERROR;
  public static systems.zlink.framework.configuration.ZLinkLogLevel[] values();
  public static systems.zlink.framework.configuration.ZLinkLogLevel valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkMeshNodeBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMeshChannelBuilder channel(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder listen(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder listen();
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder listen(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setBindHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setAdvertiseHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setRoutingId(systems.zlink.contracts.core.RoutingId);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setRoutingIdPrefix(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setPlacementWeight(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setActorCapacity(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setSpotCapacity(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setActivationConcurrency(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setInstanceSpotIdleTimeout(java.time.Duration);
  public abstract systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder objects();
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeSocketConfig configureRouterSocket();
  public abstract systems.zlink.framework.configuration.ZLinkSpotPublisherConfig configureSpotPublisher();
  public abstract systems.zlink.framework.configuration.ZLinkMeshPeerConnections peerConnections();
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setDefaultRequestTimeout(java.time.Duration);
  public abstract <THandler, TMessage> systems.zlink.framework.configuration.ZLinkMeshNodeBuilder addRouteSendHandler(java.lang.Class<THandler>, java.lang.Class<TMessage>);
  public abstract <THandler, TRequest, TReply> systems.zlink.framework.configuration.ZLinkMeshNodeBuilder addRouteRequestHandler(java.lang.Class<THandler>, java.lang.Class<TRequest>, java.lang.Class<TReply>);
}
public interface systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMeshObjectClientBuilder client();
  public abstract systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder server();
}
public interface systems.zlink.framework.configuration.ZLinkMeshObjectClientBuilder {
}
public interface systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addEntrySpot(java.lang.Class<? extends systems.zlink.framework.spots.ZLinkEntrySpot<?>>);
  public abstract <TSpot extends systems.zlink.framework.spots.ZLinkSpot<?>> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addSpotFactory(java.lang.String, java.lang.Class<TSpot>, java.util.function.Consumer<systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot>>);
  public abstract <TSpot extends systems.zlink.framework.spots.ZLinkInstanceSpot> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addInstanceSpotFactory(java.lang.String, java.lang.Class<TSpot>, java.util.function.Consumer<systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder<TSpot>>);
  public abstract <TActor extends systems.zlink.framework.actors.ZLinkActor> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addActorFactory(java.lang.String, java.lang.Class<TActor>, java.lang.Class<? extends systems.zlink.framework.actors.ZLinkActorFactory>, java.util.function.Consumer<systems.zlink.framework.configuration.ZLinkActorFactoryBuilder<TActor>>);
}
public final class systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode> {
  public static final systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode SPOT_WIDE;
  public static final systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode PER_ACTOR;
  public static systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode[] values();
  public static systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode valueOf(java.lang.String);
  public int value();
}
public final class systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode> {
  public static final systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode ANY_TURN_BOUNDARY;
  public static final systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode APPLICATION_SIGNALED;
  public static systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode[] values();
  public static systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkActorFactoryBuilder<TActor extends systems.zlink.framework.actors.ZLinkActor> {
  public abstract void disableRelocation();
  public abstract void recreateOnRelocation();
  public abstract void preserveStateWith(java.lang.Class<? extends systems.zlink.framework.actors.ZLinkActorRelocationAdapter<TActor>>);
}
public interface systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot extends systems.zlink.framework.spots.ZLinkSpot<?>> {
  public abstract systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot> stableTypeLimit(int);
  public abstract systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot> executionMode(systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode);
  public abstract systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot> relocationReadiness(systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode);
  public abstract void disableRelocation();
  public abstract void recreateOnRelocation();
  public abstract void preserveStateWith(java.lang.Class<? extends systems.zlink.framework.spots.ZLinkSpotRelocationAdapter<TSpot>>);
}
public interface systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder<TSpot extends systems.zlink.framework.spots.ZLinkInstanceSpot> {
  public abstract systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder<TSpot> stableTypeLimit(int);
  public abstract void disableRelocation();
  public abstract void recreateOnRelocation();
  public abstract void preserveStateWith(java.lang.Class<? extends systems.zlink.framework.spots.ZLinkSpotRelocationAdapter<TSpot>>);
}
public final class systems.zlink.framework.configuration.ZLinkMessageFlowOutcome extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkMessageFlowOutcome> {
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome RECEIVED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome DISPATCHED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome REPLIED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome DROPPED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome SENT;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome REPLY_RECEIVED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome ERROR;
  public static systems.zlink.framework.configuration.ZLinkMessageFlowOutcome[] values();
  public static systems.zlink.framework.configuration.ZLinkMessageFlowOutcome valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.spring.EnableZLinkFramework extends java.lang.annotation.Annotation {
}
public interface systems.zlink.framework.spring.ZLinkMetricsCustomizer {
  public abstract void customize(io.micrometer.core.instrument.MeterRegistry);
}
```

## Spring Bean Contract

The Spring starter provides `ZLinkFrameworkRuntime`, `ZLinkRouteMeshRuntime`,
`ZLinkClientServerRuntime`, and `ZLinkFanoutRuntime` as singleton beans.
The three topology beans register exactly the object the runtime's
corresponding accessor returns, so they don't create a new adapter or
separate runtime. The public client and remaining runtime service beans
also use the object owned by the same `ZLinkFrameworkRuntime`.

Service sockets, discovery loops, and application workers aren't started
while creating the bean. `SmartLifecycle.start()` calls the same
runtime's start exactly once. The contract guaranteed to the application
is the public bean's type, singleton lifetime, and reference identity.
The auto-configuration class, bean factory methods, lifecycle adapters,
and their constructors are implementation details, not the application
public signature.

Core's internal bootstrap package is qualified-exported only to the
starter module. Compilation must fail if an application calls
`ZLinkFrameworkRuntime.start(...)` from the module path or classpath. A
contract test calls the bootstrap from the actual module path and
confirms the runtime reaches `SERVING`.

The contract test resolves each of the four runtime beans twice to
confirm they're singletons. It then compares the three topology beans
with `assertSame` against the return values of
`runtime.routeMeshRuntime()`, `runtime.clientServerRuntime()`, and
`runtime.fanoutRuntime()`. The lifecycle regression test confirms the
service socket isn't started before or after bean creation, and that
start and shutdown are each delivered once to the same
`ZLinkFrameworkRuntime`.
