# Node.js 기초 타입과 구성 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Node.js 계약 목차](../README.ko.md)

이 문서는 ZLink Framework에서 `@zlink-systems/framework`와
`@zlink-systems/nestjs`가 내보내는 기초 타입과 구성 관련 정확한 TypeScript declaration을 고정한다.
동작 의미는 [공통 스펙](../../../../README.ko.md)이 소유하며, 이 문서는 이름, generic, overload,
상속, member, parameter와 반환형만 정의한다.

## 1. 공통 식별자와 직렬화 utility

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

## 2. 등록, topology와 relocation builder

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

Entry Spot 등록은 구현 type만 받는다. Entry Spot의 `SpotId`는 Framework가
`<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식으로 발급한다. caller가 fixed `RoutingId`나
`SpotId`를 지정하는 option은 제공하지 않는다.

`channel(channelName)` 뒤에는 `client()` 또는 `server()`를 정확히 한 번 호출한다. Client builder에는
역할별 추가 설정이 없고 Server builder만 `setWeight(...)`와 handler 등록을 제공한다. 따라서 잘못된 역할
설정을 runtime validation까지 미루지 않고 TypeScript type 단계에서 막는다. Server membership이 없는
MeshNode도 시작할 수 있다. `addClientServerChannel(channelName)`의
client는 send/request를 시작하고
server는 수신한 send/request 처리와 reply만 수행한다. ClientServer builder에서는 `client()`와
`server()` 중 하나 또는 둘 다 호출할 수 있지만 각 역할은 최대 한 번만 등록한다. 같은 ChannelName의
두 역할은 `(ChannelName, Role)` key의 별도 registration으로 하나의 topology를 공유하고 같은 역할의
중복은 startup 오류다. RouteMesh 역할 단일 선택과 [ChannelName](../../../../01-glossary.ko.md#channelname) 충돌 규칙은 바꾸지 않는다.

Automatic [RouteMesh](../../../../01-glossary.ko.md#routemesh)는 RID를 canonical byte order로 비교하고 더 작은 RID의 [MeshNode](../../../../01-glossary.ko.md#meshnode)만 상대 endpoint로
connect한다. Manual topology는 application endpoint 구성에 따라 한쪽 또는 양쪽에서 connect할 수 있다.
양쪽 연결이나 automatic discovery 경합·오래된 snapshot으로 중복 후보가 생기면 handshake와 admission이
같은 RID와 lifecycle generation을 확인해 하나만 ready 상태로 유지한다.

두 MeshNode가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이 없을 때만 peer
connection이 필요하지 않다. Channel Client membership만 등록한 경우도 같다. 어느 한쪽에라도 weight
`0`을 포함한 Channel Server membership이 있으면 연결을 만들고 liveness를 유지한다. ClientServer와
classic fanout registration은 별도 물리 topology이므로 이 판정에 포함하지 않는다.

Client는 manual endpoint와 location store [automatic discovery](../../../../01-glossary.ko.md#automatic-discovery)를 함께 사용할 수 있다. 두 source가 같은
Server RID와 [lifecycle generation](../../../../01-glossary.ko.md#lifecycle-generation)을 가리키면 connection intent와 ready target을 하나로 합친다. Automatic과
manual 모두 Client만 server로 connect하며 Server는 client endpoint를 찾거나 outbound connect를 시작하지
않는다.

같은 process에 Client와 Server를 모두 등록하면 listener와 service admission을 마친 local Server도 remote
Server와 같은 candidate 집합에 포함한다. [Ready](../../../../01-glossary.ko.md#ready), positive weight, non-draining 조건을 동일하게 적용하며
local 우선순위나 remote 제외는 없다. Local Server를 선택해도 Client DEALER에서 Server ROUTER로 실제
transport message를 전달하고 handler 직접 호출로 codec, HWM, timeout, cancellation, correlation 또는
terminal completion을 우회하지 않는다.

Factory configure callback은 option과 relocation policy를 한 builder에서 설정한다. Callback은
`disableRelocation()`, `recreateOnRelocation()`, `preserveStateWith(...)` 중 정확히 하나를 호출해야 한다.
누락하거나 둘 이상 호출하면 socket bind 전에 configuration error다. Actor builder는 Actor adapter,
User·Instance Spot builder는 Spot adapter만 받는다.

`ZLinkUserSpotExecutionMode.PerActor`는 `recreateOnRelocation()`만 허용한다.
다른 policy를 함께 등록하면 socket bind 전에
startup configuration error다. PerActor Spot은 stateless execution shell이며 Actor
policy와 adapter가 Actor state를 각각 처리한다. 유지해야 하는 shared state와
Spot-level schedule은 application의 Redis·database·service 같은 외부 저장소에 둔다.
Target runtime-private shell은 같은 public Spot ID와 object generation을 사용하며
Spot authority 전에는 public lookup에 노출하지 않는다. Authority 전환 뒤 `ToSpot`,
Create와 Join은 target, `ToActor`는 Actor별 current owner를 사용한다. Stale source
route는 operation identity, generation, deadline, correlation과 reply route를 보존해
relay한다. Actor queue seal부터 target admission까지 1초는 운영 목표이며 초과해도
relocation을 취소하거나 rollback하지 않는다.

`relocationReadiness`를 생략하면 `AnyTurnBoundary`다. `ApplicationSignaled`는
`SpotWide`에서만 허용하며 `PerActor`와 함께 등록하면 socket bind 전에
startup configuration error다. Spot callback은 optional이며 없으면 no-op으로 처리한다.

Adapter는 application state를 `Uint8Array` opaque bytes로만 주고받으며 typed state, 별도 contract identifier와
message wrapper를 사용하지 않는다. Framework는 `preserveStateWith(...)`의 cross-node materialization에서만 adapter를
호출한다. Maintenance 이관, remote User·Entry Spot join과 whole User Spot relocation의 각 Actor participant에는
Actor adapter를 사용한다. Whole User Spot의 Spot root와 cross-node User·Instance Spot materialization에는 [Spot](../../../../01-glossary.ko.md#spot)
adapter를 사용한다. Same-node join·relocation에서는 adapter를 호출하지 않으며 `DisableRelocation` cross-node operation은
`capture(...)` 전에 거부한다. `RecreateOnRelocation` policy도 application payload를 capture·restore하지 않는다.

Target은 owner commit 전에 restore와 accepted journal staging을 완료하며 application handler를 실행하지
않는다. Owner commit과 lifecycle callback 뒤 저장된 기존 작업을 실제 queue에 먼저 넣고 relocation
temporary queue 작업을 그 뒤에 옮긴다. Temporary queue 등록을 제거하고 dispatch를 atomic하게 전환한 뒤
target을 `"ready"`로 연다. Source cleanup, `"completed"` 기록과 bound-session 위치 갱신 응답은 target
message 처리를 막지 않는다. Infrastructure relocation은 Entry Spot의 join·leave callback을 호출하지 않는다.
`"ready"` 뒤 target process가 종료되면 ordinary owner loss로 처리하며 이전 relocation payload를 자동
replay하지 않는다. 이 barrier를 조작하는 public phase API는 제공하지 않는다.

같은 source와 target process 안의 재시도에서 factory와 `restore(...)`를 두 번 이상 호출할 수 있다.
`capture(...)`도 [authority](../../../../01-glossary.ko.md#authority) commit 전에 반복될 수 있다. Current owner와 attempt fence만 completion을 commit하고
admission을 열 수 있다. Callback에는 relocation ID를 추가하지 않으므로 application restore와 capture는 retry-safe해야
하며 exactly-once external side effect를 보장하지 않는다. `capture(...)`가 throw하거나 rejected Promise로
끝나면 relocation attempt를 게시하지 않고 durable abort와 source normalization 뒤 admission을 복원한다.
`restore(...)` 실패는 target staging을 폐기하고 source owner를 유지한 채 같은 target process에서 동일한
payload로 다시 시도할 수 있다. 다른 target을 자동 선택하지 않는다.
Framework는 capture 결과를 즉시 복사하고 restore마다 독립된 `Uint8Array`를 전달한다.
Adapter가 비동기 호출 뒤 payload를 보관하려면 직접 복사해야 한다.

`capture(...)`가 반환한 `Uint8Array`는 최대 64 MiB이며 길이가 0인 것은 유효한 application payload다. JavaScript runtime에서
`null`, `undefined` 또는 `Uint8Array`가 아닌 값을 반환하면 adapter failure로 처리하며 빈 payload로 바꾸지 않는다.
Framework는 resolved array를 callback 완료 직후 복사하므로 adapter는 완료 뒤 그 배열을 변경해도 저장된 payload에
영향을 주지 않는다. 각 restore attempt는 factory가 새로 만든 instance와 독립된 새 `Uint8Array` copy를 받는다.
실패한 instance나 이전 attempt의 payload array를 다음 attempt에 재사용하지 않는다.

Final owner·membership commit 전 `capture(...)` 또는 `restore(...)`가 throw, reject 또는 잘못된 반환값으로
끝나고 허용된 attempt를 모두 사용하면 `StateIncompatible`로 분류한다. Operation deadline 때문에 callback을
취소하면 `DeadlineExceeded`를 사용하고 stale attempt cancellation은 terminal result를 commit하지 못한다. Source
capture failure는 durable abort 뒤 reversible seal을 해제하고 target restore failure는 staging instance를
폐기한다. 모든 target이 실패하기 전에는 [deadline](../../../../01-glossary.ko.md#deadline) 안에서 replacement를 시도할 수 있다. Relocation Store, authority
CAS, recovery transport와 teardown failure는 adapter failure가 아니며 해당
phase의 `StoreUnavailable`, `RelocationFailed` 또는 `TeardownFailed`로 분류한다.

Entry Spot과 `PerActor` User Spot의 Actor maintenance는 application membership
callback을 호출하지 않는다. Actor state·queue·timer를 복원하고 Authority·membership을
commit한 뒤 target message 처리를 시작한다. Bound Session 위치 갱신 응답은 target 처리를
막지 않으며, 응답 전에는 source relay가 이전 route의 message를 target에 전달한다. `PerActor` Spot policy는 `RecreateOnRelocation`만 허용하고 Spot
adapter를 등록하지 않는다.

Relocated terminal reply accounting은 internal command ID 46 `replyRelayAck`를 사용한다. 이 command는 stable
relocation ID, operation ID, exact request-source fence(owner ID, lease generation, node RID, node generation)와
status만 가지며 payload와 metadata를 싣지 않는다. Physical connection close는 terminal 증거가 아니다. ACK 또는
accepted record에 저장한 exact request-source lease expiry만 terminal accounting을 완료하며 public ACK API는 없다.

`mailboxMessageBudget`와 `mailboxByteBudget`은 owner별 application mailbox의 메시지 수와 byte 합계
상한이며 startup 전에만 설정한다. Byte 회계는 payload 크기만 세지 않는다 —
`payload 크기 + metadata 크기 + 작업당 고정 비용`을 더한다. Payload가 비어 있어도 작업 하나는 `0` byte가
아니며, 큰 payload에서도 고정 비용은 그대로 더한다. 합이 `Number.MAX_SAFE_INTEGER`를 넘으면 그 값으로
고정하고 그 제출을 거절한다. 회계 규칙은
[Framework API §8.2](../../../../06-framework-api.ko.md#82-handler-실행-객체와-dependency-수명)가 소유한다.
`0`은 unlimited가 아니라 Framework profile의 유한 기본값을 선택한다.
음수, 정수가 아닌 값과 안전 정수 범위를 벗어난 값은 startup 설정 오류다. Logical Multicast의 local target도
이 용량 제한으로 admission을 판단한다.

`setInstanceSpotIdleTimeout(timeoutMs)`은 유휴 Instance Spot 정리 기준 시간을 millisecond로 설정한다.
기본값은 `0`이고 `0`은 정리하지 않음을 뜻한다. 허용 범위는 `0`과 양수이며 음수, 정수가 아닌 값과 안전
정수 범위를 벗어난 값은 startup 설정 오류다. 값은 MeshNode lifecycle 시작 전에 고정하고 runtime setter를
제공하지 않는다. STREAM worker의 `idleTimeoutMs`와는 별개의 설정이며 서로 값을 상속하지 않는다. 정리
대상은 Instance Spot뿐이고 Entry Spot과 User Spot은 이 설정의 영향을 받지 않는다. 유휴 판정 조건,
`ZLinkSpotCloseReason.IdleEvicted` 전달과 정리 뒤 cold activation 규칙은
[Spot 모델 §6.2](../../../../11-spot-model.ko.md#62-유휴-instance-spot-정리)가 소유한다.

Framework가 모든 registration에서 만든 fully encoded MeshNode descriptor는 1 MiB 이하여야 한다.
Spot type과 stateful object capability collection은 각각 최대 1024개다. Runtime은 완성된 descriptor를 socket
bind 전에 한 번에 검증한다. Bound를 넘으면
startup을 실패시키며 collection을 truncate·split하거나 [descriptor](../../../../01-glossary.ko.md#descriptor) 일부를 게시하지 않는다.

`configureNetwork()`의 기본 BindHost는 `127.0.0.1`이다. AdvertiseHost를 생략하면 wildcard가
아닌 [BindHost](../../../../01-glossary.ko.md#bindhost)를 사용하고, wildcard BindHost에서는 [AdvertiseHost](../../../../01-glossary.ko.md#advertisehost)를 반드시 명시한다.
Automatic discovery listener의 port를 생략하거나 listener 호출을 생략하면 port `0`을
사용한다. Listener별 host 설정은 root 기본값보다 우선한다.

MeshNode의 기본 object role은 `ZLinkObjectRole.None`이다. `objects().client()`는 global Actor·Spot client와
manager를 제공하고 `objects().server()`는 그 기능과 factory·Entry Spot hosting을 함께 제공한다. Role을 두 번
선택하거나 factory를 Server builder 밖에서 등록하면 startup configuration error다. Client 또는 Server role은
[Location Store](../../../../01-glossary.ko.md#location-store)가 필요하다.

Object Client에도 RouteMesh Channel Server를 등록할 수 있다. Application Node direct handler는 등록할 수
없으며 Object Client RID를 Node direct target으로 지정하면 다른 RID로 바꾸지 않고 `NotFound`로 끝낸다.

모든 User·Instance Spot과 Actor factory는 relocation policy를 명시해야 한다. 생략을 Disabled로 해석하지 않는다.
Factory별 capacity가 없으면 MeshNode의 object capacity를 사용한다.
Placement [weight](../../../../01-glossary.ko.md#weight)는 정수 `0..10000`이고 기본값은 `100`이다. RouteMesh
Channel Server와 ClientServer Server weight도 같은 범위와 기본값을 사용한다. 범위 밖 값은 startup 설정과
runtime 변경에서 `InvalidOperation`이다. Weighted selection은 후보 weight 합계를 최소 64-bit 정수로
계산한다. Active limit은 양수이고 pending limit은 0 이상이다.

## 3. Handler decorator와 dispatch option

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
