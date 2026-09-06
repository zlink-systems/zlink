# Node.js Spot과 Instance Spot 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Spot address와 messaging](../../../03-spot-actor/06-spot-address-messaging.ko.md) ·
[Spot·Actor membership](../../../03-spot-actor/05-spot-actor-membership.ko.md)

Bound Session의 relocation route 갱신은 [Session–Actor binding §8.2](../../../04-session/02-session-actor-binding.ko.md#82-control-message-424344)가 소유한다.

이 문서는 ZLink Framework에서 `@zlink-systems/framework`와
`@zlink-systems/nestjs`가 내보내는 Spot 관련 정확한 TypeScript declaration을 고정한다.

Location Store가 [Spot](../../../00-foundation/02-glossary.ko.md#spot)의 current owner와 lifecycle state를 확정해 보관하는 정보를 authority라 한다.
Authority가 Missing이고 caller가 Instance intent를 지정했을 때 새 Instance Spot을 준비하는 과정을
cold activation이라 한다.

## 1. Global identity와 lifecycle

`SpotId`는 UTF-8 encoded 크기 1..255 bytes의 `string`이며 [Location Store](../../../00-foundation/02-glossary.ko.md#location-store) transaction domain 전체에서
유일한 logical ID다. 비교는 case-sensitive 비교이고 Unicode normalization과 case folding을 적용하지 않는다. 일반 message는 SpotId만 받고
current [authority](../../../00-foundation/02-glossary.ko.md#authority)를 resolve한다. `SpotRef`는 지정한 incarnation을 닫을 때 사용하는 immutable location
snapshot이다.

```ts
export declare enum ZLinkSpotKind {
 Invalid = "invalid",
 Entry = "entry",
 User = "user",
 Instance = "instance"
}

export declare enum ZLinkSpotCloseReason {
 ExplicitClose = 0,
 HostShutdown = 1,
 RelocationOut = 2,
 IdleEvicted = 3
}

export interface ZLinkSpotClosingContext {
 readonly reason: ZLinkSpotCloseReason;
 readonly deadline: Date;
}

export declare enum ZLinkSpotRelocationReadyOutcome {
 Continued = 0,
 Relocated = 1
}

export interface ZLinkSpotRelocationReadyCompletion {
 readonly outcome: ZLinkSpotRelocationReadyOutcome;
}

export interface ZLinkSpotRelocationReadyCall {
 defer(): void;
}

export interface ZLinkSpotAcceptRejectResponse {
 readonly accepted: boolean;
 readonly reply?: unknown;
}

export interface ZLinkSpotActorJoinResult extends ZLinkSpotAcceptRejectResponse {}

export interface ZLinkSpotCreateResponse extends ZLinkSpotAcceptRejectResponse {}

export interface ZLinkActorCreateResponse extends ZLinkSpotAcceptRejectResponse {}

export interface ZLinkSpotActorMembershipLifecycle<TActor extends ZLinkActor = ZLinkActor> {
 onJoinedActor(actor: TActor): Promise<void>;
 onLeaveActor(actor: TActor): Promise<void>;
 onDisconnectActor?(actor: TActor): Promise<void>;
}

export interface ZLinkUserSpotActorLifecycle<TActor extends ZLinkActor = ZLinkActor>
 extends ZLinkSpotActorMembershipLifecycle<TActor> {
 onActorJoin(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult>;
}

export interface ZLinkSpot<TActor extends ZLinkActor = ZLinkActor>
 extends ZLinkUserSpotActorLifecycle<TActor> {
 readonly context: ZLinkSpotContext<TActor>;
 configure?(): void;
 onCreate?(request: ZLinkMessage): Promise<ZLinkSpotCreateResponse>;
 onInitialize?(): Promise<void>;
 onClosing?(
 context: ZLinkSpotClosingContext,
 cleanupSignal: AbortSignal): Promise<void>;
 onRelocationReadyCompleted?(
 completion: ZLinkSpotRelocationReadyCompletion): Promise<void>;
}

export interface ZLinkInstanceSpot {
 readonly context: ZLinkInstanceSpotContext;
 configure?(): void;
 onInitialize?(): Promise<void>;
 onClosing?(
 context: ZLinkSpotClosingContext,
 cleanupSignal: AbortSignal): Promise<void>;
}

export interface ZLinkSpotCommonContext<TSpot> {
 readonly meshName: string;
 readonly spotId: SpotId;
 readonly objectGeneration: bigint;
 readonly nodeRid: RoutingId;
 readonly outbound: ZLinkSpotOutbound;
 addTimer<THandler extends ZLinkSpotTimerHandler<TSpot>>(
 name: string,
 periodMs: number,
 handlerType: Type<THandler>,
 options?: ZLinkTimerOptions,
 signal?: AbortSignal): Promise<ZLinkTimer>;
 runCpuWorker<T>(work: (signal: AbortSignal) => T): ZLinkWorkerCall<T>;
 runIoWorker<T>(work: (signal: AbortSignal) => Promise<T>): ZLinkWorkerCall<T>;
}

export interface ZLinkSpotContext<
 TActor extends ZLinkActor = ZLinkActor,
 TSpot extends ZLinkSpot<TActor> = ZLinkSpot<TActor>>
 extends ZLinkSpotCommonContext<TSpot> {
 readonly handlers: ZLinkSpotHandlerRegistry;
 relocationReady(): ZLinkSpotRelocationReadyCall;
 leaveActor(actor: TActor, signal?: AbortSignal): Promise<void>;
 close(signal?: AbortSignal): Promise<boolean>;
}

export interface ZLinkInstanceSpotContext
 extends ZLinkSpotCommonContext<ZLinkInstanceSpot> {
 readonly handlers: ZLinkInstanceSpotHandlerRegistry;
 close(signal?: AbortSignal): Promise<boolean>;
}
```

`SpotId`와 `SpotRef`의 canonical declaration은
[기초 타입과 구성](01-foundation-configuration.ko.md)이 소유한다. 이 문서는 해당 타입을 다시 선언하지 않고
Spot lifecycle에서 사용하는 위치만 고정한다.

`ZLinkSpotCloseReason`의 numeric 값은 `ExplicitClose=0`, `HostShutdown=1`, `RelocationOut=2`,
`IdleEvicted=3`이다. `IdleEvicted`는 Instance Spot 전용 이유이며 Entry Spot과 User Spot에는 전달하지
않는다. 유휴 판정 조건과 정리 뒤 재활성화 규칙은
[Spot 모델 §6.2](../../../03-spot-actor/01-spot-model.ko.md#62-쓰지-않고-남아-있는-instance-spot-정리)가 소유한다.
`deadline`은 closing operation의 absolute UTC instant다. Framework는 callback invocation 전에는
`cleanupSignal`을 abort하지 않고 deadline이 끝날 때 abort한다. Entry·User·[Instance Spot](../../../00-foundation/02-glossary.ko.md#entry-spot-user-spot과-instance-spot)만 callback을 받고
Actor별 closing callback은 제공하지 않는다. Host Shutdown은 Actor membership과 local instance가 유효한
상태에서 callback을 실행하고 fulfillment 뒤 scope와 authority를 정리한다. Standalone Actor relocation은 Entry
Spot을 닫지 않으므로 이 callback을 호출하지 않는다.

`relocationReady().defer()`는 `SpotWide`와 `ApplicationSignaled`를 함께 등록한
Spot turn에서만 유효하다. Framework는 이동하지 않았거나 relay-ready reply가 accepted 상태가 되기 전에 abort했으면
source에서 `Continued`, 이동했으면 target에서 `Relocated` completion을 optional
`onRelocationReadyCompleted(...)`에 전달한다. Callback이 없으면 no-op으로 완료한다.
Callback 완료 전에는 보류한 application message와 timer를 실행하지 않는다.

기본 `FrameworkManaged`, `PerActor`, Entry·Instance Spot, Spot turn 밖과 같은 turn의
중복 `defer()`는 queue mutation 전에 `InvalidOperation`으로 실패한다. `defer()`
뒤 같은 turn의 다른 Framework operation도 같은 오류다. Recovery에서 callback이
다시 실행될 수 있으므로 구현한 callback은 retry-safe해야 한다.

## 2. Handler와 outbound

```ts
export interface ZLinkSpotHandlerRegistry extends ZLinkActorHandlerRegistry {
 addHandler<THandler>(handlerType: Type<THandler>, packetName?: string): this;
 addPacket<THandler>(handlerType: Type<THandler>): this;
 addSubscribe<THandler>(handlerType: Type<THandler>, channelName: string, topic: string): this;
}

export interface ZLinkInstanceSpotHandlerRegistry {
 addPacket<THandler>(handlerType: Type<THandler>): this;
}

export interface ZLinkSpotOutbound {
 sendToSpot(spotId: SpotId, message: unknown): ZLinkSpotSendCall;
 requestToSpot(spotId: SpotId, request: unknown): ZLinkSpotRequestCall;
 publish(channelName: string, topic: string, event: unknown): ZLinkPublishCall;
 sendToChannel(channelName: string, message: unknown): ZLinkSendCall;
 requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall;
}

export interface ZLinkSpotSendCall {
 metadata(key: string, value: string): this;
 metadata(metadata: ZLinkMessageMetadata): this;
 instanceSpot(): this;
 instanceSpot(instanceSpotType: string): this;
 inMesh(meshName: string): this;
 submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSpotRequestCall {
 metadata(key: string, value: string): this;
 metadata(metadata: ZLinkMessageMetadata): this;
 instanceSpot(): this;
 instanceSpot(instanceSpotType: string): this;
 inMesh(meshName: string): this;
 timeout(timeoutMs: number): this;
 submit<TReply>(signal?: AbortSignal): Promise<TReply>;
 yield<TReply>(signal?: AbortSignal): Promise<TReply>;
}

export interface ZLinkSpotPacketHandler<TSpot, TMessage> {
 handle(spot: TSpot, message: TMessage, context: ZLinkMessageContext): Promise<void>;
}

export declare function ZLinkSpotActorRequest(packetName?: string): MethodDecorator;
export declare function ZLinkSpotActorSend(packetName?: string): MethodDecorator;

export interface ZLinkSpotActorSendHandler<TSpot, TActor extends ZLinkActor, TMessage> {
 handle(spot: TSpot, actor: TActor, context: ZLinkMessageContext, message: TMessage): Promise<void>;
}

export interface ZLinkSpotActorRequestHandler<TSpot, TActor extends ZLinkActor, TRequest, TReply> {
 handle(spot: TSpot, actor: TActor, context: ZLinkMessageContext, request: TRequest): Promise<TReply>;
}
```

Node runtime은 Spot packet·request·subscription·timer handler class를 Spot activation마다
한 번 만들고 재사용한다. Actor send·request handler class는 Actor activation마다 한
번 만들고 재사용한다. 서로 다른 Actor는 handler instance와 activation-scoped provider를
공유하지 않는다. Nest provider의 singleton·request·transient 설정으로 이 수명을
바꾸거나 handler lifetime option을 추가하지 않는다.

Same-node Join은 Actor handler를 유지한다. Cross-node Join과 relocation은 source
handler를 정리하고 target activation에서 다시 만든다. Handler field에 복구해야 하는
state를 두지 않으며 Spot 또는 Actor가 소유한다.

## 3. Manager와 single-use create call

```ts
export interface ZLinkSpotCreateResult {
 readonly spot: SpotRef;
 readonly state: ZLinkSpotCreateState;
 readonly reply?: unknown;
}

export declare enum ZLinkSpotCreateState {
 Existing = "existing",
 Created = "created",
 Rejected = "rejected"
}

export interface ZLinkSpotManager {
 create(spotType: string): ZLinkSpotCreateCall;
 getOrCreate(
 spotId: SpotId,
 spotType: string): ZLinkSpotGetOrCreateCall;
 find(spotId: SpotId, signal?: AbortSignal): Promise<SpotRef | undefined>;
 close(spot: SpotRef, signal?: AbortSignal): Promise<boolean>;
}

export interface ZLinkSpotCreateCall {
 inMesh(meshName: string): this;
 request(request: unknown): this;
 timeout(timeoutMs: number): this;
 submit(signal?: AbortSignal): Promise<ZLinkSpotCreateResult>;
 yield(signal?: AbortSignal): Promise<ZLinkSpotCreateResult>;
}

export interface ZLinkSpotGetOrCreateCall {
 inMesh(meshName: string): this;
 request(request: unknown): this;
 timeout(timeoutMs: number): this;
 submit(signal?: AbortSignal): Promise<ZLinkSpotCreateResult>;
 yield(signal?: AbortSignal): Promise<ZLinkSpotCreateResult>;
}
```

Entry·User·Instance SpotId는 UTF-8 encoded 크기 1..255 bytes의 global string key다. Stable type은 UTF-8 1..255 bytes이며 case-sensitive 값 비교로
비교하고 normalization하지 않는다. Object generation은 positive signed-63-bit 값이다. MeshName과 NodeRid는
조회 시점의 route [snapshot](../../../00-foundation/02-glossary.ko.md#snapshot)이며 identity key에 포함하지 않는다.

Create와 GetOrCreate call은 single-use다. 같은 option을 두 번 설정하거나 terminal
`submit(...)`을 두 번 호출하면 `InvalidOperation`이다. User Spot `create`는 Framework가 새 global Spot ID를 발급한다.
`getOrCreate`는 같은 User kind·[stable type](../../../00-foundation/02-glossary.ko.md#stable-type)의
ready Spot을 `existing`으로 반환한다. Creating이면 authority 변경을 기다리고, Ready가
되면 `existing`, cleanup으로 Missing이 되면 새 reservation을 경쟁한다. Kind나 type이 다르면
`TypeMismatch`, [deadline](../../../00-foundation/02-glossary.ko.md#deadline) 안에 terminal state가 되지 않으면 `DeadlineExceeded`다.

`close(spotRef)`는 지정한 incarnation만 닫는다. Generation이 다르면 `InvalidOperation`, 이동 중이면
`Unavailable`이다. Framework는 current ref를 다시 찾아 다른 incarnation을 닫지 않는다.

Instance Spot에는 manager create·get-or-create를 제공하지 않는다.

### Instance Spot cold activation과 첫 message

`sendToSpot`과 `requestToSpot`은 global SpotId를 받고 `ZLinkSpotSendCall` 또는
`ZLinkSpotRequestCall`을 반환한다. Marker overload는 `instanceSpot()`과
`instanceSpot(instanceSpotType: string)`이며, Mesh 입력은 `inMesh(meshName: string)`이다.
Send의 `submit(signal?: AbortSignal)`은 `Promise<void>`, request의
`submit<TReply>(signal?: AbortSignal)`·`yield<TReply>(signal?: AbortSignal)`는
`Promise<TReply>`를 반환한다. 정확한 선언은 [§2](#2-handler와-outbound)에 있다.

Cold activation의 type·Mesh 선택, 생성 순서와 최초 message 보존은
[Spot address messaging §4](../../../03-spot-actor/06-spot-address-messaging.ko.md#4-cold-activation--message로-instance-spot을-처음-만드는-방법)가 소유한다. 완료 경계는
[Spot address messaging §5](../../../03-spot-actor/06-spot-address-messaging.ko.md#5-existing-owner를-향한-direct-call과-완료-경계)를 따른다.

User Spot의 `close(spotRef)`는 active Actor [membership](../../../00-foundation/02-glossary.ko.md#membership)이 남아 있으면 `false`를 반환한다. Framework는 Actor를
자동으로 leave·destroy하지 않는다. One-way Spot message는 local outbound admission까지만 기다리며, target
queue admission 이후 실패한 operation을 새 owner에게 hidden retry하지 않는다.

Instance Spot factory는 actor-free lifecycle만 만든다. Actor handler, Actor membership과 Logical Multicast
subscription을 등록할 수 없으며 direct packet과 timer만 처리한다. Instance Spot은 handler나 timer가 자신의
context `close(...)`를 호출해 종료한다. 일반 message는 missing RID에 새 intent를 만들거나 factory를 직접
시작하지 않는다.

Stored creation intent의 재개 범위와 steady `Ready` owner 실패의 구분은
[Object lifecycle §3](../../../03-spot-actor/09-object-lifecycle.ko.md#3-없는-객체를-언제-만드는가)가 소유한다.

다음 예제에서 `spotClient`는 `ZLinkSpotOutbound`이고 `cartId`는 호출할 global SpotId다. Instance
intent를 명시했으므로 Spot이 없을 때만 cold activation에 필요한 stable type과 최초 Mesh를 사용한다.

```ts
const reply = await spotClient
 .requestToSpot(cartId, request)
 .instanceSpot("shopping-cart") // Missing이면 이 stable type의 Instance Spot 생성을 요청한다.
 .inMesh("commerce") // Missing cold activation의 Mesh 선택 범위만 제한한다.
 .timeout(5_000)
 .submit<CartReply>(); // 생성 또는 기존 owner의 handler가 반환한 reply를 기다린다.
```

User·Instance Spot relocation에서는 Framework가 `addTimer(...)`로 만든 logical timer registration, 마지막 완료
tick sequence, 다음 예정 시각과 아직 실행하지 않은 pending tick을 relocation payload에 포함한다. Target은
logical timer registration을 복원하므로 application이 timer를 다시 등록하지 않는다. 현재 실행 중인 timer handler만 source에서
완료하고 target Ready 전에는 복원한 tick을 실행하지 않는다.

Public trace category는 `spot-instance`, `actor-relocation`다. 의미와 검증 기준은
[Spot address와 messaging](../../../03-spot-actor/06-spot-address-messaging.ko.md)과
[Spot·Actor membership](../../../03-spot-actor/05-spot-actor-membership.ko.md)이 소유한다.

이 문서에 선언된 `yield(...)`는 `SpotWide` User Spot 또는 Instance Spot의 shared turn에서만 유효하다.
Entry Spot과 `PerActor` User Spot에서 호출하면 operation을 제출하거나 turn을 반환하지 않고
`invalidConfiguration`으로 완료한다. `submit(...)`은 현재 turn을 유지하는 공통 `Async` 의미다.
