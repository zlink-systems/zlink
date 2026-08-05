# Node.js Channel, Request, And Routing Public Interface

[Interface table of contents](README.en.md) · [Node.js contract table of contents](../README.en.md)

This document fixes the exact TypeScript declarations related to
Channel, request, and routing that `@zlink-systems/framework` and
`@zlink-systems/nestjs` export in ZLink Framework. Behavioral meaning is
owned by the [common spec](../../../../README.en.md) — this document
only defines names, generics, overloads, inheritance, members,
parameters, and return types.

A provider child context is created each time a Node direct/Channel
send/request and classic fanout subscription handler runs. The handler
and filter are each created once in the same context, and use the same
scoped dependency. If a classic fanout message matches multiple
subscription handlers, a separate child context is created per handler.
Nest provider scope or application provider registration can't change
this lifetime. Once dispatch finishes, the framework cleans up the
instances it created and the child context.

## 1. Entry Spot And Classic Fanout

```ts
export declare class ZLinkEncodedPayload {
    private readonly payload;
    private constructor();
    static from(bytes: Uint8Array): ZLinkEncodedPayload;
    data(): Uint8Array;
    toBytes(): Uint8Array;
    copy(): ZLinkEncodedPayload;
    size(): number;
    isEmpty(): boolean;
    getString(encoding?: BufferEncoding): string;
    close(): void;
}

export interface ZLinkEndpointConnections {
    connect(endpoint: string): void;
    disconnect(endpoint: string): void;
    listConnections(): readonly string[];
}

export interface ZLinkEntrySpot<TActor extends ZLinkActor = ZLinkActor>
    extends ZLinkSpotActorMembershipLifecycle<TActor> {
    readonly context: ZLinkEntrySpotContext<TActor>;
    configure?(): void;
    onInitialize?(): Promise<void>;
    onClosing?(
        context: ZLinkSpotClosingContext,
        cleanupSignal: AbortSignal): Promise<void>;
    onCreateActor?(
        actor: TActor,
        createRequest: ZLinkMessage): Promise<ZLinkActorCreateResponse>;
}

export interface ZLinkEntrySpotActorRequestHandler<
    TEntrySpot extends ZLinkEntrySpot<TActor>,
    TActor extends ZLinkActor,
    TRequest,
    TReply> {
    handle(
        spot: TEntrySpot,
        actor: TActor,
        context: ZLinkMessageContext,
        request: TRequest): Promise<TReply>;
}

export interface ZLinkEntrySpotActorSendHandler<
    TEntrySpot extends ZLinkEntrySpot<TActor>,
    TActor extends ZLinkActor,
    TMessage> {
    handle(
        spot: TEntrySpot,
        actor: TActor,
        context: ZLinkMessageContext,
        message: TMessage): Promise<void>;
}

export interface ZLinkEntrySpotContext<TActor extends ZLinkActor = ZLinkActor, TEntrySpot extends ZLinkEntrySpot<TActor> = ZLinkEntrySpot<TActor>> extends ZLinkSpotCommonContext<TEntrySpot> {
    readonly handlers: ZLinkSpotHandlerRegistry;
    destroyActor(actor: TActor, signal?: AbortSignal): Promise<void>;
}

export interface ZLinkFanoutChannelBuilder {
    enablePublisher(endpoint: string): this;
    enablePublisher(port?: number): this;
    setBindHost(bindHost: string): this;
    setAdvertiseHost(advertiseHost: string): this;
    routingId(publisherRoutingId: RoutingId): this;
    setRoutingIdPrefix(prefix: string): this;
    enableSubscriber(): this;
    connect(endpoint: string): this;
    subscriberConnections(): ZLinkEndpointConnections;
}

export interface ZLinkFanoutClient {
    publish(channelName: string, event: unknown): ZLinkFanoutPublishCall;
    publish(channelName: string, topic: string, event: unknown): ZLinkFanoutPublishCall;
    getListenerStatus(channelName: string): ZLinkFanoutListenerStatus;
}

export interface ZLinkFanoutListenerStatus {
    readonly channelName: string;
    readonly endpoint: string;
    readonly observedAt: Date;
}

export interface ZLinkFanoutPublishCall {
    submit(signal?: AbortSignal): Promise<void>;
}
```

The Entry Spot ID is issued by the framework at MeshNode startup. The
application doesn't provide the Entry Spot ID as a configuration value.
Actor create finishes the selected owner
[MeshNode](../../../../01-glossary.en.md#meshnode)'s Entry Spot
membership and Actor Ready barrier in the same lifecycle. Afterward, a
one-way business message is delivered directly to the Actor queue,
without going through the Entry Spot callback.

When maintenance materializes an Actor into a target Entry Spot,
Snapshot first finishes the Actor adapter's `restore(...)`, and Recreate
finishes factory materialization without payload restore. It restores
the queue/Actor timer, commits Location authority/Entry
[membership](../../../../01-glossary.en.md#membership), and then starts
Actor message processing. The Bound Session location update is then
performed with `sessionActorLocationUpdateReqMsg` and
`sessionActorLocationUpdateResMsg` send messages, and Actor processing
doesn't stop even without a response.

Infrastructure relocation doesn't call target joined, source leave, or a
separate relocation callback. Only a regular same-node/remote User/
Entry Spot join uses the existing admission/joined callback and source
leave callback. Neither the `SpotWide` User Spot aggregate nor the
`PerActor` User Spot's Actor relocation calls a membership callback
either.

`ZLinkFanoutClient.publish(...)` provides both a call that uses the
typed event's packet name as topic, and a call that specifies
[topic](../../../../01-glossary.en.md#topic) explicitly.
`ZLinkFanoutPublishCall.submit(...)` completes normally once the local
publisher transport accepts the event. It doesn't return subscriber
count or receipt completion. `ZLinkPublishCall` is Logical-Multicast-only
and isn't used for classic fanout. Even with 0 subscribers, it
completes normally once the publisher local queue accepts the event.

`getListenerStatus(...)` returns the current advertised endpoint once
the publisher listener has bound. If port `0` was used in configuration,
the returned endpoint contains the actual port the operating system
chose. It fails with `ZLinkConfigurationException` if the host hasn't
started or that channel isn't registered as a publisher.

Passing the internal liveness-dedicated exact byte `01 5A 4C 46 31` to
the overload that specifies topic raises `ZLinkConfigurationException`
without starting transport. The overload that omits topic uses the
typed event's [packet name](../../../../01-glossary.en.md#packet-name),
so it doesn't create this internal topic.

A fanout publisher that registered a location store selects one of a
fixed Publisher RID or automatic allocation before startup, and
publishes a dedicated [descriptor](../../../../01-glossary.en.md#descriptor).
A publisher with no Store can be used as a target with a manually
delivered listener endpoint, but doesn't perform RID allocation or
automatic discovery registration. `enableSubscriber()` with no argument
queries the [location store](../../../../01-glossary.en.md#location-store)
for every valid publisher descriptor of the same ChannelName and
connects them all. The overload taking an endpoint configures a manual
subscriber that only uses the specified endpoint. Configuring both
subscriber modes on one channel fails startup. An automatic subscriber
needs a location store, but it isn't needed for a host that only uses a
manual publisher and manual subscriber. A publisher only publishes a
descriptor and doesn't start an outbound connect to a subscriber
endpoint. Only the subscriber connects to the publisher endpoint, and
an automatic subscriber creates one connection intent per Publisher RID
and lifecycle generation.

## 2. Metrics, Monitoring, And Packet

```ts
export interface ZLinkMetricAttributes {
    readonly [name: string]: string | number | boolean;
}

export interface ZLinkMetricHistogram {
    record(value: number, attributes?: ZLinkMetricAttributes): void;
}

export interface ZLinkMetricInstrument {
    add(value: number, attributes?: ZLinkMetricAttributes): void;
}

export interface ZLinkMetricsOptions {
    readonly meterProvider?: ZLinkMeterProvider;
}

export interface ZLinkOutboundRouteConfig {
    targetNodeRid: RoutingId;
    endpoint: string;
}

export declare function ZLinkPacket(packetName: string): ClassDecorator;
```

The Node runtime also records Instance Spot observations with
`ZLinkMeter`. The
[Instance Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
instrument name catalog used in this language is the following six
values, and the name/kind/unit and attribute restrictions are owned by
[runtime-metrics](../../../../25-runtime-metrics.en.md).

- `zlink.instance_spot.activations`
- `zlink.instance_spot.activation.duration`
- `zlink.instance_spot.pending.messages`
- `zlink.instance_spot.pending.bytes`
- `zlink.instance_spot.claim.conflicts`
- `zlink.instance_spot.takeovers`

A one-way placement/activation failure is recorded in
`zlink.mesh_node.messages.dropped` with `surface=instance_spot`
attached. `ZLinkMessageFlowEvent` also doesn't add a separate event ID —
it uses `eventId=zlink.message_flow`, the same surface, and
`outcome=dropped`. Only the bounded type registered at startup is
recorded in `instanceSpotType`, and
[Spot ID](../../../../01-glossary.en.md#spot-id),
[owner](../../../../01-glossary.en.md#owner) ID, and internal authority
fields aren't used as metric attributes. `eventId=zlink.message_flow`'s
reason only uses `ZLinkMessageFlowReason` values, and
`eventId=zlink.dispatch_error`'s reason only uses
`ZLinkDispatchErrorReason` values.

## 3. Location Peer And Logical Multicast

```ts
export declare function ZLinkPublish(packetName?: string): MethodDecorator;

export interface ZLinkPublishCall {
    metadata(key: string, value: string): this;
    metadata(metadata: ZLinkMessageMetadata): this;
    submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkPublishMessageContext extends ZLinkMessageContext {
    readonly channelName: string;
    readonly topic: string;
    readonly source?: string;
}

export interface ZLinkFanoutHandler<TMessage> {
    handle(message: TMessage, context: ZLinkPublishMessageContext): Promise<void>;
}
```

The omitted `pageSize` is 100. An explicit value must be an integer in
range `1..1000`, and the continuation token is an opaque value only the
provider interprets.

## 4. Request And RouteMesh Client

```ts
export declare function ZLinkRequest(packetName?: string): MethodDecorator;

export interface ZLinkRequestCall {
    metadata(key: string, value: string): this;
    metadata(metadata: ZLinkMessageMetadata): this;
    timeout(timeoutMs: number): this;
    submit<TReply>(signal?: AbortSignal): Promise<TReply>;
}

export interface ZLinkChannelRequestCall {
    metadata(key: string, value: string): this;
    metadata(metadata: ZLinkMessageMetadata): this;
    timeout(timeoutMs: number): this;
    submit<TReply>(signal?: AbortSignal): Promise<TReply>;
    yield<TReply>(signal?: AbortSignal): Promise<TReply>;
}

export interface ZLinkRequestHandler<TRequest, TResponse> {
    handle(request: TRequest, context: ZLinkMessageContext): Promise<TResponse>;
}

export interface ZLinkRouteClient {
    sendToNode(meshName: string, targetNodeRid: RoutingId, message: unknown): ZLinkSendCall;
    requestToNode(meshName: string, targetNodeRid: RoutingId, request: unknown): ZLinkRequestCall;
    sendToChannel(channelName: string, message: unknown): ZLinkSendCall;
    requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall;
    sendToSpot(spotId: SpotId, message: unknown): ZLinkSpotSendCall;
    requestToSpot(spotId: SpotId, request: unknown): ZLinkSpotRequestCall;
}

export interface ZLinkRouteConfig {
    channelName: string;
    endpoint: string;
}

export interface ZLinkRouteMeshRuntimeOptions {
    mesh(meshName: string): ZLinkMeshPlacementRuntimeOptions;
    channel(channelName: string): ZLinkMeshChannelRuntimeOptions;
}

export interface ZLinkMeshPlacementRuntimeOptions {
    placementWeight: number;
}

export interface ZLinkMeshChannelRuntimeOptions {
    weight: number;
}
```

In the following example, `client` is a `ZLinkRouteClient` obtained
through configuration or dependency injection. It starts a request with
[ChannelName](../../../../01-glossary.en.md#channelname), and the
Promise `submit(...)` returns waits until the terminal reply.

```ts
const reply = await client
    .requestToChannel("checkout", request) // the framework selects one of the Server candidates.
    .timeout(5_000)                        // specifies this request operation's timeout in ms.
    .submit<CheckoutReply>();              // receives the terminal reply as CheckoutReply.
```

`maxMessageSize` is only set before startup, and a runtime property
isn't provided. `0` normalizes to the maximum complete message size the
binding or transport can receive. If transport is unlimited, it uses the
service wire's `uint32` representation limit minus envelope overhead. A
positive value can't exceed that representation limit — exceeding it is
rejected as a startup configuration error. Peers exchange the
normalized value in the internal handshake, and sender and receiver each
apply the smaller of the two values as the effective bound before
complete message allocation. A public option for this negotiation isn't
provided.

## 5. Route Handler And One-Way Submit

```ts
export interface ZLinkRouteRequestHandler<TRequest, TReply> {
    handle(request: TRequest, context: ZLinkRouteMessageContext): Promise<TReply>;
}

export interface ZLinkRouteMessageContext extends ZLinkMessageContext {
    readonly meshName: string;
    readonly sourceNodeRid: RoutingId;
}

export interface ZLinkRouteSendHandler<TMessage> {
    handle(message: TMessage, context: ZLinkRouteMessageContext): Promise<void>;
}

export declare function ZLinkSend(packetName?: string): MethodDecorator;

export interface ZLinkSendCall {
    metadata(key: string, value: string): this;
    metadata(metadata: ZLinkMessageMetadata): this;
    submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSendHandler<TMessage> {
    handle(message: TMessage, context: ZLinkMessageContext): Promise<void>;
}
```

Every server one-way call's `submit(signal?)` and session Actor
`relay(...)` don't produce a normal-completion value. Normal completion
means the source-local queue the operation family defines accepted the
message. It doesn't wait for remote handler execution, subscriber
receipt, remote Spot queue admission, or application callback
completion. If queue capacity is insufficient, it waits for a capacity
signal up to that family's send timeout, and submits the message
exactly once if room opens up within the deadline. `Backpressured` isn't
a public terminal result or an immediately raised application exception.
The Promise rejects with `DeadlineExceeded` on timeout, `Unavailable` on
a route break, and `ShuttingDown` on runtime shutdown. Absence of an
Actor/Spot/Mesh/session target uses `NotFound`.

If `AbortSignal` is already aborted before `submit(...)` or `relay(...)`,
runtime admission isn't started, and it rejects with `AbortError`. Once
admission has started, only the terminal result confirmed first among
abort, timeout, shutdown, and acceptance remains, and the same operation
isn't resubmitted after an abort or timeout. An invalid argument/handle/
state and a duplicate submit are handled as exceptional completion. A
valid first terminator of a STREAM reply atomically claims and consumes
the one-shot reply token before starting transport. If two calls created
from the same token race, the one that fails the claim doesn't attempt
transport and ends with exceptional completion. Even if the call that
consumed the token ends with `DeadlineExceeded`, runtime shutdown, or
abort, the token can't be used again. An already-used token is also
handled as exceptional completion. A STREAM reply isn't given the client
request timeout — it only uses that STREAM socket's send timeout.

RouteMesh node/Channel/Spot/Actor use the send timeout of the selected
MeshNode ROUTER, ClientServer uses the client DEALER,
[classic fanout](../../../../01-glossary.en.md#classic-fanout) uses the
publisher socket, and STREAM send/reply use that STREAM socket. A
bound session uses one framework socket send timeout even if the
local/remote Actor route changes. If there's no public setting, 1 second
is used. The millisecond setting used for one-way admission only allows
a finite integer in range `1..2147483647`. `undefined` selects the
default, and `0`, a negative value, a non-integer value, and exceeding
the cap are rejected with `ZLinkConfigurationError`.

[Logical Multicast](../../../../01-glossary.en.md#logical-multicast)'s
`ZLinkPublishCall.submit(...)` does a direct handoff to a bounded I/O
executor. If a worker slot isn't obtained immediately, it waits for
capacity up to the send timeout. After obtaining the slot but before the
publish attempt starts, abort and
[shutdown](../../../../01-glossary.en.md#shutdown) can block the
operation from starting. The moment the publish attempt starts is the
operation commit barrier — an abort after that doesn't interrupt the
already-confirmed [snapshot](../../../../01-glossary.en.md#snapshot)
operation. Once the transaction has started, an individual target
failure doesn't roll back an already-accepted target or automatically
retry the whole publish. Per-target admission/failure results of remote
transport and the local Spot queue aren't returned or aggregated into
monitoring. It completes normally even with 0 targets in the snapshot.

## 6. Serializer And STREAM Session

```ts
export interface ZLinkSession {
    readonly context: ZLinkSessionContext;
    onConnected?(context: ZLinkSessionContext): Promise<void>;
    onDisconnected?(context: ZLinkSessionContext): Promise<void>;
    onError?(context: ZLinkSessionContext, error: ZLinkStreamError): Promise<void>;
    onDispatch?(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void>;
}

export interface ZLinkSessionActor {
    readonly actorId: ActorId;
    readonly ref: ActorRef;
    relay(payload: ZLinkMessage, signal?: AbortSignal): Promise<void>;
    relay(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage,
        signal?: AbortSignal): Promise<void>;
    notifyDisconnected(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSessionActors {
    readonly bound: readonly ZLinkSessionActor[];
    bind(actor: ActorRef, signal?: AbortSignal): Promise<ZLinkSessionActor>;
    bindOrGet(actor: ActorRef, signal?: AbortSignal): Promise<ZLinkSessionActor>;
    find(actorId: ActorId): ZLinkSessionActor | undefined;
}

export interface ZLinkSessionClient {
    send(message: unknown): ZLinkSessionSendCall;
    reply(message: unknown): ZLinkSessionReplyCall;
}

export interface ZLinkSessionContext {
    readonly sessionId: string;
    readonly routingId?: RoutingId;
    readonly localAddr?: string;
    readonly remoteAddr?: string;
    readonly client: ZLinkSessionClient;
    readonly actors: ZLinkSessionActors;
    readonly handlers: ZLinkSessionHandlerRegistry;
    close(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSessionDispatchContext {
    readonly packetName: string;
    readonly metadata: ZLinkMessageMetadata;
    readonly canReply: boolean;
}

export interface ZLinkSessionFactory<TSession extends ZLinkSession = ZLinkSession> {
    create(context: ZLinkSessionContext): Promise<TSession>;
}

export interface ZLinkSessionHandlerRegistry {
    addHandler<THandler>(handlerType: Type<THandler>): this;
    tryHandle(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<boolean>;
}

export interface ZLinkSessionPacketHandler<TSessionContext, TMessage = ZLinkMessage> {
    handle(context: TSessionContext, dispatch: ZLinkSessionDispatchContext, message: TMessage): Promise<void>;
}

export interface ZLinkSessionReplyCall {
    compress(enabled?: boolean): this;
    submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSessionSendCall {
    metadata(key: string, value: string): this;
    metadata(metadata: ZLinkMessageMetadata): this;
    compress(enabled?: boolean): this;
    submit(signal?: AbortSignal): Promise<void>;
}
```

After bind, relay/request relay and `notifyDisconnected(...)` use the
per-Actor stored route and don't query the Location Store per message. A
physical disconnect has the framework perform an automatic all-settled
notification to every current binding, running the Spot callback at
most once per exact binding identity. `notifyDisconnected(...)` is a
logical notification while the connection is kept, and waits for the
callback terminal. A relocation route update is only allowed on the
same ObjectGeneration. After the target Actor is restored and starts
message processing, the target runtime sends
`sessionActorLocationUpdateReqMsg` and changes that Actor's route and
the current location snapshot `ZLinkSessionActor.ref` returns, together.
The snapshot reflects the same ActorId/ObjectGeneration and the target
MeshName/NodeRid. Even without a response, target Actor processing
doesn't stop, and the same request is resent at a fixed interval. The
route and physical STREAM connection of a different Actor on the same
Session that isn't included in the relocation target are kept. The
application doesn't rebind to learn about relocation.

`relay(...)` taking only payload is a one-way admission that completes
normally once the local relay queue accepts the operation. The overload
taking a dispatch context immediately transfers the explicit current
STREAM request reply capability to the runtime at call time. If
admission succeeds, the Actor typed reply completes the original STREAM
correlation terminal-once, and on admission failure the framework
completes the same correlation as a typed failure. The caller doesn't
perform a separate reply/retry. The one-way dispatch context has no
reply capability, so it only waits until local admission.

`yield(...)` declared on this document's request builder is only valid
when the caller owns the shared turn of a `SpotWide` User Spot or
Instance Spot. In a different execution context, it completes with
`invalidConfiguration`, without submitting the message or returning the
turn. `submit(...)` is the common `Async` semantics that keeps the
current turn.
