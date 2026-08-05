# Node.js Actor와 session binding 공개 인터페이스

Session에 bind된 Actor를 포함한 Spot relocation은 target에서 Actor와 queue를 복원하고 owner와
membership을 commit한 뒤 message 처리를 시작한다. Target runtime은
`sessionActorLocationUpdateReqMsg`를 send하여 binding route와 bound-session current Actor
location snapshot을 갱신한다. 응답이 없어도 Actor 처리를 멈추지 않으며 정해진 간격으로
같은 요청을 다시 보낸다. Snapshot은 target MeshName·NodeRid를 제공한다. Relocation 자체는 physical·logical disconnect가
아니므로 Actor disconnect callback을 실행하지 않는다. relocation 대상에 포함되지 않은 다른 Actor의 route와 physical connection은
변경하지 않는다.

[인터페이스 목차](README.ko.md) · [Actor model](../../../../14-actor-model.ko.md) ·
[Spot·Actor membership](../../../../15-spot-actor.ko.md)

이 문서는 ZLink Framework에서 `@zlink-systems/framework`와
`@zlink-systems/nestjs`가 내보내는 Actor 관련 정확한 TypeScript declaration을 고정한다.

## 1. Actor identity, factory와 context

`ActorId`는 Location Store transaction domain 전체에서 유일한 logical ID다. UTF-8 encoded 크기는
1..255 bytes이고 case-sensitive exact value로 비교하며 normalization하지 않는다. 일반 message는
`ActorId`만 받고 current authority를 resolve한다. `ActorRef`는 exact incarnation을 변경하거나 session에
bind할 때 사용하는 immutable location snapshot이다.

```ts
export interface ZLinkActor {
    readonly context: ZLinkActorContext;
    configure?(): void;
    onJoinCompleted?(completion: ZLinkActorJoinCompletion): Promise<void>;
}

export interface ZLinkActorContext {
    readonly actorId: ActorId;
    readonly objectGeneration: bigint;
    readonly meshName: string;
    readonly spotId?: SpotId;
    readonly boundSession: ZLinkBoundSession;
    joinSpot(spotId: SpotId): ZLinkActorJoinSpotCall;
    joinSpot(spotId: SpotId, request: unknown): ZLinkActorJoinSpotCall;
    joinEntrySpot(): ZLinkActorJoinEntrySpotCall;
    joinEntrySpot(request: unknown): ZLinkActorJoinEntrySpotCall;
}

export interface ZLinkActorFactory<TActor extends ZLinkActor = ZLinkActor> {
    create(context: ZLinkActorContext, signal?: AbortSignal): Promise<TActor>;
}

export interface ZLinkActorHandlerRegistry {
    addHandler<THandler>(handlerType: Type<THandler>, packetName?: string): this;
}

export interface ZLinkActorJoinCall<TSelf> {
    timeout(timeoutMs: number): TSelf;
    defer(): void;
}

export interface ZLinkActorJoinEntrySpotCall
    extends ZLinkActorJoinCall<ZLinkActorJoinEntrySpotCall> {}

export interface ZLinkActorJoinSpotCall
    extends ZLinkActorJoinCall<ZLinkActorJoinSpotCall> {}

export interface ZLinkActorJoinOperationId {
    readonly high: bigint;
    readonly low: bigint;
}

export type ZLinkActorJoinCompletion =
    | { readonly status: 'accepted'; readonly operationId: ZLinkActorJoinOperationId;
        readonly actor: ActorRef; readonly reply?: ZLinkMessage }
    | { readonly status: 'rejected'; readonly operationId: ZLinkActorJoinOperationId;
        readonly reply?: ZLinkMessage }
    | { readonly status: 'failed'; readonly operationId: ZLinkActorJoinOperationId;
        readonly kind: ZLinkFrameworkErrorKind };
```

`ActorId`와 `ActorRef`의 canonical declaration은
[기초 타입과 구성](01-foundation-configuration.ko.md)이 소유한다. 이 문서는 Actor lifecycle과 manager가
그 타입을 사용하는 정확한 위치만 고정한다.

## 2. Global client와 manager

```ts
export interface ZLinkActorClient {
    sendToActor(actorId: ActorId, message: unknown): ZLinkActorSendCall;
    requestToActor(actorId: ActorId, request: unknown): ZLinkActorRequestCall;
}

export interface ZLinkActorManager {
    create(actorId: ActorId, actorType: string): ZLinkActorCreateCall;
    getOrCreate(actorId: ActorId, actorType: string): ZLinkActorGetOrCreateCall;
    find(actorId: ActorId, signal?: AbortSignal): Promise<ActorRef | undefined>;
    findSpot(actorId: ActorId, signal?: AbortSignal): Promise<SpotRef | undefined>;
    destroy(actor: ActorRef, signal?: AbortSignal): Promise<boolean>;
}

export interface ZLinkActorCreateCall {
    inMesh(meshName: string): this;
    request(request: unknown): this;
    timeout(timeoutMs: number): this;
    submit(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
    yield(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
}

export interface ZLinkActorGetOrCreateCall {
    inMesh(meshName: string): this;
    request(request: unknown): this;
    timeout(timeoutMs: number): this;
    submit(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
    yield(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
}

export type ZLinkActorCreateResult =
    | { readonly status: 'existing'; readonly actor: ActorRef }
    | {
        readonly status: 'created';
        readonly actor: ActorRef;
        readonly reply?: unknown;
      }
    | { readonly status: 'rejected'; readonly reply?: unknown };

export interface ZLinkActorRequestCall {
    metadata(key: string, value: string): this;
    timeout(timeoutMs: number): this;
    submit<TReply>(signal?: AbortSignal): Promise<TReply>;
    yield<TReply>(signal?: AbortSignal): Promise<TReply>;
}

export interface ZLinkActorSendCall {
    metadata(key: string, value: string): this;
    submit(signal?: AbortSignal): Promise<void>;
}
```

`create`와 `getOrCreate`가 반환하는 call은 single-use다. 같은 option을 두 번 설정하거나
terminal `submit(...)`을 두 번 호출하면 `InvalidOperation`이다. `inMesh(...)`를
생략했는데 eligible Mesh가 둘 이상이면 `InvalidOperation`, object-role Mesh가 하나도 없으면
`NotConfigured`다. 명시한 Mesh가 없으면 `NotFound`다.
Caller는 target RID나 predicate를 지정하지 않는다.

`create`는 같은 ActorId의 ready incarnation이 있으면 `AlreadyExists`, stable type이 다르면
`TypeMismatch`다. 새 attempt는 `created` 또는 `rejected`를 반환한다.
`getOrCreate`는 같은 type의 [ready](../../../../01-glossary.ko.md#ready) Actor를
callback 없이 `existing`으로 반환한다. Creating이면 authority 변경을 기다리고 CAS loser는
별도 factory나 callback을 시작하지 않는다. 서로 다른 operation은 ready 뒤 `existing`을
받고 cleanup 뒤 새 reservation을 경쟁하며 앞선 application reply를 공유하지 않는다.
같은 source Node RID·lifecycle generation·`OperationId`의 재전송만 correlation-free
`creation-operation-terminal-v1` envelope를 읽고 현재 correlation·reply route로 reply를
다시 encode한다. Terminal은 original deadline 뒤 5분 동안 유지한다. Callback exception은 `rejected`가 아니라
typed creation failure다. 전체 deadline이
끝나면 `DeadlineExceeded`, capacity가 없으면 `CapacityExceeded`다. ActorRef의 object generation이
current와 다른 exact lifecycle operation은 `InvalidOperation`, 이동 중에는 `Unavailable`이다.

Actor create는 선택한 owner MeshNode의 Entry Spot membership과 Ready barrier를 같은 lifecycle에서 완료한다.
Ready 이후 one-way message는 Actor queue에 직접 제출한다. Resolve 또는 queue admission 이후 stale route가
확인되어도 Framework는 새 [owner](../../../../01-glossary.ko.md#owner)를 찾아 같은 operation을 hidden retry하지 않는다.

Actor Join call은 동기 `defer()`만 제공하고 `submit(...)`·`yield(...)`를 제공하지
않는다. `defer()`는 current handler에 immutable Join intent와 비활성 barrier만
등록하며 target 조회나 Store I/O를 시작하지 않는다. Handler가 정상적으로 끝나면
Join을 실행하고 실패하면 barrier를 폐기한다. Handler가 `yield(...)`를 사용한
경우에는 마지막 continuation이 끝나기 전까지 barrier를 활성화하지 않는다.

Result는 같은 operation ID의 `onJoinCompleted(...)` Actor callback으로 전달한다.
Operation ID는 completion idempotency ID이며 `RelocationId`, reservation ID나
aggregate commit ID가 아니다. Same-node와 cross-node completion retry는 current
source와 target process lifetime으로 제한한다. Process 종료 뒤 다른 runtime이
completion을 자동 replay하지 않는다.

Request 없는 overload는 empty `ZLinkMessage`를 고정한다. Timeout 기본값은 5초이고
명시 값은 millisecond 올림 기준 유한한 `1..2_147_483_647` ms다. `defer()`를
호출한 시점에 monotonic absolute deadline을 고정한다.

`ZLinkActorContext.spotId`가 없으면 Actor는 current [Entry Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) member이고 값이 있으면 해당 User Spot member다.
같은 상태를 나타내는 별도 boolean이나 mutable Spot instance를 제공하지 않는다. `findSpot(actorId)`도 current User
[Spot](../../../../01-glossary.ko.md#spot) [membership](../../../../01-glossary.ko.md#membership)만 `SpotRef`로 반환하며 Entry Spot에서는 `undefined`다. Factory는 target attempt마다 새 Actor와
context를 만들고 cross-node restore가 실패한 instance를 다음 attempt에 재사용하지 않는다.

## 3. Session binding

Session binding은 `ActorRef.actorId + objectGeneration`의 exact incarnation을 고정한다. Local Actor instance를
받는 bind overload, global Actor directory, handle resolver와 별도 ActorRef [snapshot](../../../../01-glossary.ko.md#snapshot) 변환 API는 제공하지 않는다.
`find(actorId)`는 해당 session에 이미 bind된 Actor만 조회하며 global directory가 아니다.

`boundSession`의 push는 현재 binding token이 지정하는 connection에만 보내는 one-way operation이다. 연결이
교체되거나 binding generation이 바뀌면 이전 operation을 새 connection으로 retarget하거나 hidden retry하지
않는다. Disconnect는 binding만 해제하며 Actor와 Spot membership은 유지한다.

Public trace category는 `actor-relocation`다. 의미와 검증 기준은
[Actor model](../../../../14-actor-model.ko.md), [Spot·Actor membership](../../../../15-spot-actor.ko.md),
[Session Actor dispatch](../../../../20-session-actor-dispatch.ko.md)가 소유한다.

Actor request에 선언된 `yield(...)`는 현재 Actor handler가 `SpotWide` User Spot의 shared execution
gate에서 실행 중일 때만 유효하다. Entry Spot Actor와 `PerActor` User Spot의 Actor가 호출하면 operation을
제출하거나 turn을 반환하지 않고 `invalidConfiguration`으로 완료한다. Actor Join은
동기 `defer()`만 제공하며 `submit(...)`과 `yield(...)`를 제공하지 않는다.
