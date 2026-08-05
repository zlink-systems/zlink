# Node.js Foundation Types And Configuration Public Interface

[Interface table of contents](README.en.md) · [Node.js contract table of contents](../README.en.md)

This document fixes the exact TypeScript declarations related to
foundation types and configuration that `@zlink-systems/framework` and
`@zlink-systems/nestjs` export in ZLink Framework. Behavioral meaning is
owned by the [common spec](../../../../README.en.md) — this document
only defines names, generics, overloads, inheritance, members,
parameters, and return types.

## 1. Common Identifiers And Serialization Utility

```ts
export type ActorId = string;
export type SpotId = string;

export interface ActorRef {
    readonly actorId: ActorId;
    readonly objectGeneration: bigint;
    readonly meshName: string;
    readonly nodeRid: RoutingId;
}

export interface SpotRef {
    readonly spotId: SpotId;
    readonly objectGeneration: bigint;
    readonly meshName: string;
    readonly nodeRid: RoutingId;
}

export declare enum ZLinkObjectRole {
    None = "none",
    Client = "client",
    Server = "server"
}

export declare enum ZLinkUserSpotExecutionMode {
    SpotWide = "spot_wide",
    PerActor = "per_actor"
}

export declare enum ZLinkSpotRelocationReadinessMode {
    AnyTurnBoundary = "any_turn_boundary",
    ApplicationSignaled = "application_signaled"
}

export declare enum ZLinkApplicationHwmProfile {
    Compact = "compact",
    LowLatency = "low_latency",
    Balanced = "balanced",
    Throughput = "throughput"
}

export interface ZLinkInboundDispatchOptions {
    applicationHwmBytes(value: bigint | undefined): this;
    applicationHwmProfile(value: ZLinkApplicationHwmProfile): this;
    processMemoryLimitBytes(value: bigint | undefined): this;
}

export declare enum ZLinkFrameworkErrorKind {
    NotFound = 0,
    AlreadyExists = 1,
    TypeMismatch = 2,
    NotConfigured = 3,
    Rejected = 4,
    Unavailable = 5,
    CapacityExceeded = 6,
    DeadlineExceeded = 7,
    ShuttingDown = 8,
    ProtocolError = 9,
    InvalidOperation = 10,
    DataLost = 11,
    InternalFailure = 12
}

export declare class ZLinkFrameworkException extends Error {
    readonly kind: ZLinkFrameworkErrorKind;
}

export declare function isZLinkMessage(value: unknown): value is ZLinkMessage;

export declare const MESSAGE_FLOW_MODE_RANK: Record<ZLinkMessageFlowLogMode, number>;

export type RoutingId = string;

export type Type<T = unknown> = new (...args: never[]) => T;
```

## 2. Registration, Topology, And Relocation Builder

```ts
export interface ZLinkActorRelocationAdapter<TActor extends ZLinkActor> {
    capture(actor: TActor, signal: AbortSignal): Promise<Uint8Array>;
    restore(actor: TActor, payload: Uint8Array, signal: AbortSignal): Promise<void>;
}

export interface ZLinkSpotRelocationAdapter<TSpot extends ZLinkSpot | ZLinkInstanceSpot> {
    capture(spot: TSpot, signal: AbortSignal): Promise<Uint8Array>;
    restore(spot: TSpot, payload: Uint8Array, signal: AbortSignal): Promise<void>;
}

export interface ZLinkActorFactoryBuilder<TActor extends ZLinkActor> {
    disableRelocation(): void;
    recreateOnRelocation(): void;
    preserveStateWith(adapterType: Type<ZLinkActorRelocationAdapter<TActor>>): void;
}

export interface ZLinkUserSpotFactoryBuilder<TSpot extends ZLinkSpot> {
    stableTypeLimit(limit: number): this;
    executionMode(mode: ZLinkUserSpotExecutionMode): this;
    relocationReadiness(mode: ZLinkSpotRelocationReadinessMode): this;
    disableRelocation(): void;
    recreateOnRelocation(): void;
    preserveStateWith(adapterType: Type<ZLinkSpotRelocationAdapter<TSpot>>): void;
}

export interface ZLinkInstanceSpotFactoryBuilder<TSpot extends ZLinkInstanceSpot> {
    stableTypeLimit(limit: number): this;
    disableRelocation(): void;
    recreateOnRelocation(): void;
    preserveStateWith(adapterType: Type<ZLinkSpotRelocationAdapter<TSpot>>): void;
}

export interface ZLinkBoundSession {
    send(message: unknown): ZLinkBoundSessionSendCall;
    disconnect(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkBoundSessionSendCall {
    metadata(key: string, value: string): this;
    submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkChannelClient {
    sendToChannel(channelName: string, message: unknown): ZLinkSendCall;
    requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall;
}

export interface ZLinkMeshPeerConnection {
    readonly endpoint: string;
    readonly expectedRoutingId?: RoutingId;
}

export interface ZLinkMeshPeerConnections {
    connect(endpoint: string): void;
    connect(expectedRoutingId: RoutingId, endpoint: string): void;
    disconnect(endpoint: string): void;
    listConnections(): readonly ZLinkMeshPeerConnection[];
}

export interface ZLinkMeshChannelBuilder {
    client(): ZLinkMeshChannelClientBuilder;
    server(): ZLinkMeshChannelServerBuilder;
}

export interface ZLinkMeshChannelClientBuilder {
}

export interface ZLinkMeshChannelServerBuilder {
    setWeight(weight: number): this;
    addHandlerGroup(groupName: string): this;
    addSendHandler<TMessage>(handlerType: Type<ZLinkSendHandler<TMessage>>): this;
    addRequestHandler<TRequest, TReply>(handlerType: Type<ZLinkRequestHandler<TRequest, TReply>>): this;
}

export interface ZLinkMeshNodeSocketConfig {
    maxMessageSize: number;
    sendHighWaterMark: bigint;
    receiveHighWaterMark: bigint;
    mailboxMessageBudget: number;
    mailboxByteBudget: number;
    receiveTimeoutMs?: number;
    sendTimeoutMs?: number;
}

export interface ZLinkSpotPublisherConfig {
    sendHighWaterMark: bigint;
    sendTimeoutMs?: number;
    lingerMs?: number;
}

export interface ZLinkMeshNodeBuilder {
    channel(channelName: string): ZLinkMeshChannelBuilder;
    listen(endpoint: string): this;
    listen(port?: number): this;
    setBindHost(bindHost: string): this;
    setAdvertiseHost(advertiseHost: string): this;
    routingId(routingId: RoutingId): this;
    setRoutingIdPrefix(prefix: string): this;
    setPlacementWeight(weight: number): this;
    setActorLimit(limit: number): this;
    setSpotLimit(limit: number): this;
    setActivationConcurrency(limit: number): this;
    setInstanceSpotIdleTimeout(timeoutMs: number): this;
    objects(): ZLinkMeshObjectRoleBuilder;
    configureRouterSocket(): ZLinkMeshNodeSocketConfig;
    configureSpotPublisher(): ZLinkSpotPublisherConfig;
    peerConnections(): ZLinkMeshPeerConnections;
    setDefaultRequestTimeout(timeoutMs: number): this;
    addRouteSendHandler<TMessage>(handlerType: Type<ZLinkRouteSendHandler<TMessage>>): this;
    addRouteRequestHandler<TRequest, TReply>(handlerType: Type<ZLinkRouteRequestHandler<TRequest, TReply>>): this;
}

export interface ZLinkMeshObjectRoleBuilder {
    client(): ZLinkMeshObjectClientBuilder;
    server(): ZLinkMeshObjectServerBuilder;
}

export interface ZLinkMeshObjectClientBuilder {
}

export interface ZLinkMeshObjectServerBuilder {
    addEntrySpot<TEntrySpot extends ZLinkEntrySpot>(entrySpotType: Type<TEntrySpot>): this;
    addSpotFactory<TSpot extends ZLinkSpot>(
        spotType: string,
        implementation: Type<TSpot>,
        configure: (builder: ZLinkUserSpotFactoryBuilder<TSpot>) => void): this;
    addInstanceSpotFactory<TSpot extends ZLinkInstanceSpot>(
        instanceSpotType: string,
        implementation: Type<TSpot>,
        configure: (builder: ZLinkInstanceSpotFactoryBuilder<TSpot>) => void): this;
    addActorFactory<TActor extends ZLinkActor>(
        actorType: string,
        factoryType: Type<ZLinkActorFactory<TActor>>,
        configure: (builder: ZLinkActorFactoryBuilder<TActor>) => void): this;
}

export interface ZLinkNetworkOptions {
    bindHost: string;
    advertiseHost?: string;
}

export interface ZLinkClientServerChannelRoleBuilder {
    client(): ZLinkClientServerChannelClientBuilder;
    server(): ZLinkClientServerChannelServerBuilder;
}

export interface ZLinkClientServerChannelClientBuilder {
    connect(endpoint: string): this;
}

export interface ZLinkClientServerChannelServerBuilder {
    listen(port?: number): this;
    setBindHost(bindHost: string): this;
    setAdvertiseHost(advertiseHost: string): this;
    setWeight(weight: number): this;
    addHandlerGroup(groupName: string): this;
    addSendHandler<TMessage>(handlerType: Type<ZLinkSendHandler<TMessage>>): this;
    addRequestHandler<TRequest, TReply>(handlerType: Type<ZLinkRequestHandler<TRequest, TReply>>): this;
}

export interface ZLinkCodecExtension {
    register(codecs: ZLinkCodecRegistrar): void;
}

export interface ZLinkCodecRegistrar {
    addSerializer(contentType: string, serializer: ZLinkMessageSerializer): this;
    addStreamCodec(contentType: string, codec: unknown): this;
}

export interface ZLinkCodecRegistryBuilder {
    use(extension: ZLinkCodecExtension): this;
}
```

Entry Spot registration only takes the implementation type. The Entry
Spot's `SpotId` is issued by the framework in the format
`<prefix>-entry-<lowercase-canonical-uuid-v4>`. An option for the
caller to specify a fixed `RoutingId` or `SpotId` isn't provided.

After `channel(channelName)`, exactly one of `client()` or `server()`
is called. The Client builder has no additional per-role setting, and
only the Server builder provides `setWeight(...)` and handler
registration. So an incorrect role configuration is blocked at the
TypeScript type stage instead of being deferred to runtime validation.
A MeshNode with no Server membership can also start. On
`addClientServerChannel(channelName)`, client starts send/request, and
server only performs processing of received send/request and reply. The
ClientServer builder can call one or both of `client()` and `server()`,
but each role is registered at most once. The two roles of the same
ChannelName share one topology through separate registrations under the
`(ChannelName, Role)` key, and duplicating the same role is a startup
error. The RouteMesh single-role-selection and
[ChannelName](../../../../01-glossary.en.md#channelname) conflict rule
don't change.

An automatic [RouteMesh](../../../../01-glossary.en.md#routemesh)
compares RID in canonical byte order, and only the
[MeshNode](../../../../01-glossary.en.md#meshnode) with the smaller RID
connects to the counterpart endpoint. A manual topology can connect
from one or both sides depending on application endpoint configuration.
If bidirectional connection or automatic discovery contention/a stale
snapshot creates a duplicate candidate, handshake and admission check
the same RID and lifecycle generation and keep only one in ready state.

A peer connection isn't needed only when both MeshNodes are Object
Client and neither has RouteMesh Channel Server membership. The same
applies when only Channel Client membership is registered. If either
side has Channel Server membership, including weight `0`, the
connection is made and liveness is kept. ClientServer and classic
fanout registration are separate physical topologies, so they aren't
included in this judgment.

A Client can use manual endpoint and location store
[automatic discovery](../../../../01-glossary.en.md#automatic-discovery)
together. If the two sources point to the same Server RID and
[lifecycle generation](../../../../01-glossary.en.md#lifecycle-generation),
the connection intent and ready target are merged into one. In both
automatic and manual, only Client connects to server — Server doesn't
look for a client endpoint or start an outbound connect.

If both Client and Server are registered on the same process, a local
Server that finished listener and service admission is also included
in the same candidate set as a remote Server. The same
[Ready](../../../../01-glossary.en.md#ready), positive weight, and
non-draining conditions apply — there's no local priority or remote
exclusion. Even when a local Server is selected, the actual transport
message is delivered from the Client DEALER to the Server ROUTER,
without bypassing codec, HWM, timeout, cancellation, correlation, or
terminal completion via a direct handler call.

The factory configure callback sets options and relocation policy on
one builder. The callback must call exactly one of
`disableRelocation()`, `recreateOnRelocation()`, `preserveStateWith(...)`.
Omitting it or calling more than one is a configuration error before
socket bind. The Actor builder only takes an Actor adapter, and the
User/Instance Spot builder only takes a Spot adapter.

`ZLinkUserSpotExecutionMode.PerActor` only allows
`recreateOnRelocation()`. Registering a different policy together is a
startup configuration error before socket bind. A PerActor Spot is a
stateless execution shell, and the Actor policy and adapter each handle
Actor state. Shared state and Spot-level schedules that must be kept
are placed in an external store the application owns, such as Redis or
a database. The target's runtime-private shell uses the same public
Spot ID and object generation, and isn't exposed to public lookup
before Spot authority. After the authority switch, `ToSpot`, Create,
and Join use the target, and `ToActor` uses the current owner per
Actor. A stale source route is relayed while preserving operation
identity, generation, deadline, correlation, and reply route. The
1-second window from Actor queue seal to target admission is an
operational goal — exceeding it doesn't cancel or roll back the
relocation.

If `relocationReadiness` is omitted, it's `AnyTurnBoundary`.
`ApplicationSignaled` is only allowed with `SpotWide`, and registering
it together with `PerActor` is a startup configuration error before
socket bind. The Spot callback is optional, and treated as a no-op if
absent.

The adapter exchanges application state only as `Uint8Array` opaque
bytes, and doesn't use typed state, a separate contract identifier, or
a message wrapper. The framework only calls the adapter in
`preserveStateWith(...)`'s cross-node materialization. An Actor adapter
is used for maintenance handoff, remote User/Entry Spot join, and each
Actor participant of a whole User Spot relocation. A
[Spot](../../../../01-glossary.en.md#spot) adapter is used for the Spot
root of a whole User Spot and cross-node User/Instance Spot
materialization. The adapter isn't called on a same-node join/
relocation, and a `DisableRelocation` cross-node operation is rejected
before `capture(...)`. A `RecreateOnRelocation` policy also doesn't
capture/restore the application payload.

The target finishes restore and accepted journal staging before the
owner commit, without running an application handler. After the owner
commit and lifecycle callback, the saved existing work is put on the
actual queue first, and the relocation temporary queue's work is moved
after that. Once temporary queue registration is removed and dispatch
is switched atomically, the target opens as `"ready"`. Source cleanup,
the `"completed"` record, and the bound-session location update
response don't block the target's message processing. Infrastructure
relocation doesn't call the Entry Spot's join/leave callback. If the
target process terminates after `"ready"`, it's handled as ordinary
owner loss, and the previous relocation payload isn't automatically
replayed. A public phase API for manipulating this barrier isn't
provided.

On a retry within the same source and target process, factory and
`restore(...)` can be called more than once. `capture(...)` can also be
repeated before the [authority](../../../../01-glossary.en.md#authority)
commit. Only the current owner and attempt fence can commit completion
and open admission. Since the callback doesn't add a relocation ID,
application restore and capture must be retry-safe, and exactly-once
external side effect isn't guaranteed. If `capture(...)` throws or ends
with a rejected Promise, the relocation attempt isn't published, and
admission is restored after durable abort and source normalization. A
`restore(...)` failure discards the target staging and keeps the source
owner, and can be retried with the same payload on the same target
process. A different target isn't automatically selected. The
framework immediately copies the capture result, and passes an
independent `Uint8Array` per restore. If the adapter needs to keep the
payload after the async call, it must copy it directly.

The `Uint8Array` `capture(...)` returns is at most 64 MiB, and a
zero-length one is a valid application payload. If the JavaScript
runtime returns `null`, `undefined`, or a non-`Uint8Array` value, it's
treated as an adapter failure and isn't turned into an empty payload.
The framework copies the resolved array right after the callback
completes, so the adapter changing that array after completion doesn't
affect the stored payload. Each restore attempt receives a new
`Uint8Array` copy independent of the instance the factory newly
created. A failed instance's or a previous attempt's payload array
isn't reused in the next attempt.

If `capture(...)` or `restore(...)` ends with a throw, reject, or an
invalid return value before the final owner/membership commit, and
every allowed attempt is used up, it's classified as
`StateIncompatible`. If the framework cancels a callback due to the
operation deadline, `DeadlineExceeded` is used, and stale attempt
cancellation can't commit a terminal result. A source capture failure
releases the reversible seal after durable abort, and a target restore
failure discards the staging instance. Before every target fails, a
replacement can be attempted within the
[deadline](../../../../01-glossary.en.md#deadline). Relocation Store,
authority CAS, recovery transport, and teardown failure aren't adapter
failures — they're classified as `StoreUnavailable`,
`RelocationFailed`, or `TeardownFailed` for that phase.

Actor maintenance on Entry Spot and a `PerActor` User Spot doesn't call
an application membership callback. It restores Actor state/queue/timer,
commits Authority/membership, and then starts target message
processing. The Bound Session location update response doesn't block
target processing, and before the response, source relay delivers a
message on the previous route to the target. The `PerActor` Spot policy
only allows `RecreateOnRelocation` and doesn't register a Spot adapter.

Relocated terminal reply accounting uses internal command ID 46
`replyRelayAck`. This command only has a stable relocation ID,
operation ID, exact request-source fence (owner ID, lease generation,
node RID, node generation), and status — it doesn't carry payload or
metadata. A physical connection close isn't terminal evidence. Only the
exact request-source lease expiry stored in an ACK or accepted record
completes terminal accounting — there's no public ACK API.

`mailboxMessageBudget` and `mailboxByteBudget` are the caps on message
count and byte sum for the per-owner application mailbox, set only
before startup. Byte accounting doesn't count only payload size — it
adds `payload size + metadata size + a fixed per-job cost`. Even if
payload is empty, one job isn't `0` bytes, and even for a large
payload, the fixed cost is still added. If the sum exceeds
`Number.MAX_SAFE_INTEGER`, it's pinned to that value and that submit is
rejected. The accounting rule is owned by
[Framework API §8.2](../../../../06-framework-api.en.md#82-handler-execution-object-and-dependency-lifetime).
`0` isn't unlimited — it selects the Framework profile's finite
default. A negative value, a non-integer value, and a value outside
the safe integer range are startup configuration errors. A Logical
Multicast local target also judges admission using this capacity
limit.

`setInstanceSpotIdleTimeout(timeoutMs)` sets the reference time for
cleaning up an idle Instance Spot, in milliseconds. The default is `0`,
and `0` means no cleanup. The allowed range is `0` and positive values
— a negative value, a non-integer value, and a value outside the safe
integer range are startup configuration errors. The value is fixed
before the MeshNode lifecycle starts, and a runtime setter isn't
provided. It's a separate setting from the STREAM worker's
`idleTimeoutMs`, and they don't inherit each other's value. Only
Instance Spot is a cleanup target — Entry Spot and User Spot aren't
affected by this setting. The idle judgment condition, the delivery of
`ZLinkSpotCloseReason.IdleEvicted`, and the cold-activation rule after
cleanup are owned by
[Spot Model §6.2](../../../../11-spot-model.en.md#62-cleaning-up-an-idle-instance-spot).

The fully encoded MeshNode descriptor the framework builds from every
registration must be at most 1 MiB. Spot type and stateful object
capability collection are each at most 1024. The runtime validates the
completed descriptor all at once before socket bind. Exceeding the
bound fails startup — it doesn't truncate/split the collection or
publish part of the [descriptor](../../../../01-glossary.en.md#descriptor).

`configureNetwork()`'s default BindHost is `127.0.0.1`. If
AdvertiseHost is omitted, a non-wildcard
[BindHost](../../../../01-glossary.en.md#bindhost) is used, and for a
wildcard BindHost, [AdvertiseHost](../../../../01-glossary.en.md#advertisehost)
must be specified. If the automatic discovery listener's port is
omitted, or the listener call itself is omitted, port `0` is used. A
per-listener host setting takes priority over the root default.

A MeshNode's default object role is `ZLinkObjectRole.None`.
`objects().client()` provides a global Actor/Spot client and manager,
and `objects().server()` provides that capability plus factory/Entry
Spot hosting. Selecting the role twice, or registering a factory
outside the Server builder, is a startup configuration error. Client or
Server role needs a [Location Store](../../../../01-glossary.en.md#location-store).

A RouteMesh Channel Server can also be registered on an Object Client.
An application Node direct handler can't be registered, and specifying
an Object Client RID as a Node direct target ends as `NotFound` without
switching to a different RID.

Every User/Instance Spot and Actor factory must specify a relocation
policy. Omission isn't interpreted as Disabled. If there's no
per-factory capacity, the MeshNode's object capacity is used. Placement
[weight](../../../../01-glossary.en.md#weight) is an integer `0..10000`,
defaulting to `100`. RouteMesh Channel Server and ClientServer Server
weight also use the same range and default. An out-of-range value is
`InvalidOperation` in both startup config and runtime change. Weighted
selection computes the sum of candidate weight using at least a 64-bit
integer. Active limit is positive, and pending limit is at least 0.

## 3. Handler Decorator And Dispatch Option

```ts
export declare function ZLinkHandlerGroup(groupName: string): ClassDecorator;

export interface ZLinkLocationOptionValues {
    readonly ownerLeaseRenewIntervalMs: number;
    readonly ownerLeaseTtlMs: number;
    readonly pollingIntervalMs: number;
    readonly storeFailureGraceMs: number;
    readonly ownerLeaseFencingMarginMs: number;
    readonly ownerLeaseRenewTimeoutMs: number;
}

export declare const zlinkDefaultLocationOptions: Readonly<ZLinkLocationOptionValues>;

export interface ZLinkDiagnosticsOptions {
    messageFlow: ZLinkMessageFlowLogMode;
    sampleRate: number;
    includeMessageSizes: boolean;

    logFile?: string;

    label?: string;
}

export type ZLinkMessageSurface =
    | "node" | "channel" | "spot" | "instance_spot" | "logical_multicast"
    | "actor" | "stream" | "classic_fanout" | "actor_relocation";
export type ZLinkMessageKind =
    | "send" | "request" | "response" | "error" | "publish" | "control";
export type ZLinkMessageFlowOutcome =
    | "succeeded" | "failed" | "backpressured" | "dropped" | "cancelled" | "shutdown";
export type ZLinkMessageFlowReason =
    | "backpressure" | "stale_target" | "target_closed" | "shutdown"
    | "location_unavailable" | "activation_rejected" | "activation_timeout";
export type ZLinkDispatchErrorReason =
    | "no_handler" | "decode_error" | "handler_exception" | "invalid_frame"
    | "reply_path_missing" | "unexpected_reply" | "backpressure" | "stale_target" | "shutdown";
export type ZLinkDispatchErrorAction = "reply_error" | "fail_caller" | "drop";

export interface ZLinkDispatchOptions {
    readonly unhandled: ZLinkUnhandledDispatchOptions;
    readonly diagnostics: ZLinkDiagnosticsOptions;
}

export interface ZLinkDispatchOptionsBuilder {
    setMessageFlowObserver(observerType: Type<ZLinkMessageFlowObserver>): this;
    setRuntimeErrorSink(sinkType: Type<ZLinkRuntimeErrorSink>): this;

    messageFlow(mode: ZLinkMessageFlowLogMode): this;
    traceSampleRate(rate: number): this;
    includeMessageSizes(include: boolean): this;

    traceLogFile(path: string): this;

    traceLabel(label: string): this;
}

```
