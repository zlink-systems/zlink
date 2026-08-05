# Node.js Channel, request와 routing 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Node.js 계약 목차](../README.ko.md)

이 문서는 ZLink Framework에서 `@zlink-systems/framework`와
`@zlink-systems/nestjs`가 내보내는 Channel, request와 routing 관련 정확한 TypeScript declaration을 고정한다.
동작 의미는 [공통 스펙](../../../../README.ko.md)이 소유하며, 이 문서는 이름, generic, overload,
상속, member, parameter와 반환형만 정의한다.

Node direct·Channel send/request와 classic fanout 구독 handler를 실행할 때마다 provider child
context를 하나 만든다. Handler와 filter는 같은 context에서 한 번씩 만들며 같은 scoped dependency를
사용한다. Classic fanout message가 여러 구독 handler와 일치하면 handler마다 별도 child context를
만든다. Nest provider scope나 Application provider 등록으로 이 수명을 바꿀 수 없다. Dispatch가 끝나면
Framework가 만든 instance와 child context를 정리한다.

## 1. Entry Spot과 classic fanout

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

Entry Spot ID는 Framework가 MeshNode startup에서 발급한다. 애플리케이션은 Entry Spot ID를 구성값으로
제공하지 않는다. Actor create는 선택한 owner [MeshNode](../../../../01-glossary.ko.md#meshnode)의 Entry Spot membership과 Actor Ready barrier를 같은
lifecycle에서 완료한다. 이후 one-way 업무 message는 Actor queue로 직접 전달되며 Entry Spot callback을
경유하지 않는다.

Maintenance가 Actor를 target Entry Spot에 materialize할 때 Snapshot은 Actor adapter
`restore(...)`를 먼저 완료하고 Recreate는 payload restore 없이 factory
materialization을 완료한다. Queue·Actor timer를 복원하고 Location authority·Entry
[membership](../../../../01-glossary.ko.md#membership)을 commit한 뒤 Actor message 처리를
시작한다. Bound Session 위치 갱신은 그 뒤 `sessionActorLocationUpdateReqMsg`와
`sessionActorLocationUpdateResMsg` send message로 수행하며 응답이 없어도 Actor 처리를
멈추지 않는다.

Infrastructure relocation은 target joined, source leave 또는 별도 relocation
callback을 호출하지 않는다. 일반 same-node·remote User·Entry Spot join만 기존
admission·joined callback과 source leave callback을 사용한다. `SpotWide` User Spot
aggregate와 `PerActor` User Spot의 Actor relocation도 membership callback을
호출하지 않는다.

`ZLinkFanoutClient.publish(...)`는 typed event의 packet name을 topic으로 사용하는 호출과
[topic](../../../../01-glossary.ko.md#topic)을 명시하는 호출을 함께 제공한다.
`ZLinkFanoutPublishCall.submit(...)`은 local publisher transport가 event를 수락하면 정상 완료한다.
Subscriber 수와 수신 완료는 반환하지 않는다. `ZLinkPublishCall`은 Logical Multicast 전용이며 classic
fanout에 사용하지 않는다. Subscriber가 0개여도 publisher local queue가 event를 수락하면 정상 완료한다.

`getListenerStatus(...)`는 publisher listener가 bind한 뒤 현재 advertised endpoint를
반환한다. 설정에 port `0`을 사용했으면 반환되는 endpoint에는 operating system이
선택한 실제 port가 들어간다. host가 시작되지 않았거나 해당 channel이 publisher로
등록되지 않았으면 `ZLinkConfigurationException`으로 실패한다.

Topic을 명시하는 overload에 내부 liveness용 exact byte `01 5A 4C 46 31`을 전달하면 transport를 시작하지
않고 `ZLinkConfigurationException`을 발생시킨다. Topic을 생략한 overload는 typed event의 [packet name](../../../../01-glossary.ko.md#packet-name)을
사용하므로 이 내부 topic을 만들지 않는다.

Location store를 등록한 fanout publisher는 고정 Publisher RID와 자동 할당 중 하나를 startup 전에
선택하고 전용 descriptor를 게시한다. Store가 없는 publisher는 listener endpoint를 수동으로 전달하는
대상으로 사용할 수 있지만 RID allocation과 automatic discovery 등록은 수행하지 않는다. 인자 없는
`enableSubscriber()`는 같은 ChannelName의 유효한 publisher [descriptor](../../../../01-glossary.ko.md#descriptor)를 [location store](../../../../01-glossary.ko.md#location-store)에서 조회해 모두
연결한다. Endpoint를 받는 overload는 명시한 endpoint만 사용하는 manual subscriber를 구성한다. 한
channel에서 두 subscriber mode를 함께 설정하면 startup이 실패한다. Automatic subscriber는 location
store가 필요하고, manual publisher와 manual subscriber만 사용하는 host에는 필요하지 않다.
Publisher는 descriptor만 게시하고 subscriber endpoint로 outbound connect를 시작하지 않는다. Subscriber만
publisher endpoint로 connect하며 automatic subscriber는 Publisher RID와 lifecycle generation마다 connection
intent 하나를 만든다.

## 2. Metrics, monitoring과 packet

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

Node runtime은 Instance Spot 관측값도 `ZLinkMeter`로 기록한다. 이 언어에서 사용하는 [Instance Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot)
계기 이름 카탈로그는 다음 여섯 값이며, 이름·종류·단위와 attribute 제한은
[runtime-metrics](../../../../25-runtime-metrics.ko.md)가 소유한다.

- `zlink.instance_spot.activations`
- `zlink.instance_spot.activation.duration`
- `zlink.instance_spot.pending.messages`
- `zlink.instance_spot.pending.bytes`
- `zlink.instance_spot.claim.conflicts`
- `zlink.instance_spot.takeovers`

One-way placement·activation 실패는 `zlink.mesh_node.messages.dropped`에
`surface=instance_spot`을 붙여 기록한다. `ZLinkMessageFlowEvent`도 별도 event ID를 추가하지 않고
`eventId=zlink.message_flow`, 같은 surface와 `outcome=dropped`를 사용한다. `instanceSpotType`에는 startup에
등록한 bounded type만 기록하며 [Spot ID](../../../../01-glossary.ko.md#spot-id), [owner](../../../../01-glossary.ko.md#owner) ID와 internal authority fields는 metric attribute로 사용하지
않는다. `eventId=zlink.message_flow`의 reason은 `ZLinkMessageFlowReason`,
`eventId=zlink.dispatch_error`의 reason은 `ZLinkDispatchErrorReason` 값만 사용한다.

## 3. Location peer와 Logical Multicast

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

생략한 `pageSize`는 100이다. 명시한 값은 `1..1000` 범위의 정수여야 하며 continuation token은 provider만
해석하는 opaque value다.

## 4. Request와 RouteMesh client

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

다음 예제에서 `client`는 구성이나 dependency injection으로 얻은 `ZLinkRouteClient`다. [ChannelName](../../../../01-glossary.ko.md#channelname)으로
요청을 시작하며, `submit(...)`이 반환한 Promise는 terminal reply까지 기다린다.

```ts
const reply = await client
    .requestToChannel("checkout", request) // Server 후보 중 하나를 Framework가 선택한다.
    .timeout(5_000)                        // 이 request operation의 timeout을 ms 단위로 지정한다.
    .submit<CheckoutReply>();              // terminal reply를 CheckoutReply로 받는다.
```

`maxMessageSize`는 startup 전에만 설정하며 실행 중 property를 제공하지 않는다. `0`은 binding 또는
transport가 수신할 수 있는 최대 complete message 크기로 정규화한다. Transport가 unlimited이면 service
wire의 `uint32` 표현 한계에서 envelope overhead를 뺀 값을 사용한다. 양수는 그 표현 한계를 넘을 수 없으며
넘으면 startup 설정 오류로 거부한다. Peer는 정규화한 값을 내부 handshake로 교환하고 sender와 receiver는
두 값 중 작은 effective bound를 complete message allocation 전에 적용한다. 이 negotiation을 위한 public
option은 제공하지 않는다.

## 5. Route handler와 one-way submit

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

모든 server one-way call의 `submit(signal?)`과 session Actor `relay(...)`는 정상 완료 값을 만들지 않는다.
정상 완료는 operation family가 정의한 source-local queue가 message를 수락했다는 뜻이다. Remote handler 실행,
subscriber 수신, remote Spot queue 수락과 application callback 완료는 기다리지 않는다. Queue capacity가
부족하면 해당 family의 send timeout까지 capacity signal을 기다리고, deadline 안에 공간이 생기면 message를
정확히 한 번 제출한다. `Backpressured`는 public terminal result나 즉시 발생하는 application exception이
아니다. Timeout은 `DeadlineExceeded`, route 단절은 `Unavailable`, runtime 종료는
`ShuttingDown`으로 Promise를 reject한다. Actor·Spot·Mesh·session target 부재는 `NotFound`를 사용한다.

`AbortSignal`이 `submit(...)` 또는 `relay(...)` 전에 이미 abort 상태이면 runtime admission을 시작하지 않고
`AbortError`로 reject한다.
Admission이 시작된 뒤에는 abort, timeout, shutdown과 수락 중 먼저 확정된 terminal 결과만 남기며, abort나
timeout 뒤에 같은 operation을 다시 제출하지 않는다. 잘못된 argument·handle·state와 중복 submit은 exceptional completion으로
처리한다. STREAM reply의 유효한 첫 terminator는 transport를 시작하기 전에 one-shot reply token을 원자적으로
claim하고 소비한다. 같은 token에서 만든 두 call이 경쟁하면 claim에 실패한 call은 transport를 시도하지 않고
exceptional completion으로 끝난다. Token을 소비한 call이 `DeadlineExceeded`, runtime shutdown 또는 abort로 끝나도 token을
다시 사용할 수 없다. 이미 사용한 token도 exceptional completion으로 처리한다. STREAM reply는 client request
timeout을 전달받지 않으며 해당 STREAM socket의 send timeout만 사용한다.

RouteMesh node·Channel·Spot·Actor는 선택한 MeshNode ROUTER, ClientServer는 client DEALER, [classic fanout](../../../../01-glossary.ko.md#classic-fanout)은
publisher socket, STREAM send·reply는 해당 STREAM socket의 send timeout을 사용한다. Bound session은
local·remote Actor route가 바뀌어도 framework socket send timeout 하나를 사용한다. 공개 설정이 없으면
1초를 사용한다. One-way admission에 사용하는 millisecond 설정은 `1..2147483647` 범위의 유한 정수만
허용한다. `undefined`는 기본값을 선택하며 `0`, 음수, 정수가 아닌 값과 상한 초과는
`ZLinkConfigurationError`로 거부한다.

[Logical Multicast](../../../../01-glossary.ko.md#logical-multicast)의
`ZLinkPublishCall.submit(...)`은 bounded I/O executor에 direct handoff한다. 즉시 worker slot을 얻지 못하면
send timeout까지 capacity를 기다린다. Slot을 얻은 뒤 publish attempt가 시작되기 전에는 abort와
[shutdown](../../../../01-glossary.ko.md#shutdown)이 operation 시작을 막을 수 있다. Publish attempt를 시작한
시점이 operation commit barrier이며, 그 뒤의 abort는 이미 확정한
[snapshot](../../../../01-glossary.ko.md#snapshot) operation을 중단하지 않는다. Transaction이 시작된 뒤
개별 target 실패는 이미 수락한 target을 rollback하거나 전체 publish를 자동 재시도하지 않는다. Remote
transport와 local Spot queue의 target별 수락·실패 결과는 반환하거나 monitoring에 집계하지 않는다.
Target snapshot이 0개여도 정상 완료한다.

## 6. Serializer와 STREAM session

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

Bind 뒤 relay·request relay와 `notifyDisconnected(...)`는 Actor별 저장 route를 사용하며 message마다
Location Store를 조회하지 않는다. Physical disconnect는 Framework가 current binding 전체에 automatic
all-settled 통지를 수행하고 exact binding identity마다 Spot callback을 최대 한 번 실행한다.
`notifyDisconnected(...)`는 connection이 유지된 상태의 logical notification이며 callback terminal까지
기다린다. Relocation route update는 같은 ObjectGeneration에만 허용한다. Target Actor가
복원되어 message 처리를 시작한 뒤 target runtime이 `sessionActorLocationUpdateReqMsg`를
send하여 해당 Actor route와 `ZLinkSessionActor.ref`가 반환하는 current location snapshot을
함께 바꾼다. Snapshot은 같은 ActorId·ObjectGeneration과 target MeshName·NodeRid를 반영한다.
응답이 없어도 Target Actor 처리를 멈추지 않으며 정해진 간격으로 같은 요청을 다시 보낸다.
같은 Session에서 relocation 대상에 포함되지 않은 다른 Actor의 route와 physical STREAM connection은 유지한다. Application은 relocation을 알기
위해 rebind하지 않는다.

Payload만 받는 `relay(...)`는 local relay queue가 operation을 수락하면 정상 완료하는 one-way admission이다.
Dispatch context를 받는 overload는 explicit current STREAM request reply capability를 호출 즉시 runtime에
이전한다. Admission에 성공하면 Actor typed reply가 original STREAM correlation을 terminal-once로 완료하고
admission failure면 Framework가 같은 correlation을 typed failure로 완료한다. Caller는 별도 reply·retry를
하지 않는다. One-way dispatch context는 reply capability가 없으므로 local admission까지만 기다린다.

이 문서의 request builder에 선언된 `yield(...)`는 호출자가 `SpotWide` User Spot 또는 Instance Spot의
shared turn을 소유할 때만 유효하다. 다른 실행 문맥에서는 message를 제출하거나 turn을 반환하지 않고
`invalidConfiguration`으로 완료한다. `submit(...)`은 현재 turn을 유지하는 공통 `Async` 의미다.
