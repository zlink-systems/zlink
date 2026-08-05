# 04. Spot instance

[레퍼런스 목차](README.ko.md)

완료 kind, capacity·timeout 규칙과 relocation 순서는
[Java 레퍼런스 04. Spot instance](../../java/reference/04-spot-instance.ko.md)와 완전히 같다.
Kotlin이 추가하는 것은 suspending 생명주기 base class(`ZLinkSuspendingSpot` 계열)와, 같은 fluent
state를 coroutine으로 감싼 `ZLinkKotlinSpotManager`/`ZLinkKotlinRouteClient` 확장이다. 정확한
signature는
[Kotlin Spot exact interface](../../common/spec/server/languages/kotlin/interfaces/spots.ko.md)가
소유한다.

---

## `ZLinkSuspendingSpot<TActor>` / `ZLinkSuspendingEntrySpot<TActor>` / `ZLinkSuspendingInstanceSpot`

Spot lifecycle을 suspend function으로 override하는 abstract base class다. Java `ZLinkSpot`/
`ZLinkEntrySpot`/`ZLinkInstanceSpot`을 구현하는 `final` bridge를 상속하므로 애플리케이션은
`*Suspending` 이름의 메서드만 override한다.

```kotlin
class RoomSpot(override val context: ZLinkSpotContext) :
    ZLinkSuspendingSpot<PlayerActor>() {

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult = ZLinkSpotActorJoinResult.accept()

    override suspend fun onJoinedActorSuspending(actor: PlayerActor) { ... }
    override suspend fun onLeaveActorSuspending(actor: PlayerActor) { ... }
}
```

**옵션.** Override 가능한 suspend 메서드는 Java 레퍼런스 04번의 Spot lifecycle 콜백과 1:1로
대응한다 — `onCreateSuspending`(`ZLinkSuspendingSpot`만), `onInitializeSuspending`,
`onClosingSuspending`, `onRelocationReadyCompletedSuspending`, `onActorJoinSuspending`
(`ZLinkSuspendingSpot`만), `onJoinedActorSuspending`, `onLeaveActorSuspending`,
`onDisconnectActorSuspending`, `onCreateActorSuspending`(`ZLinkSuspendingEntrySpot`만).

**완료 결과.** `final override fun onCreate/onInitialize/...`가 Java `CompletionStage`를 반환하는
bridge 역할만 하며, 실제 로직은 `*Suspending` suspend 메서드가 담당한다. 완료 kind와 순서는 Java
레퍼런스 04번과 같다.

**선택 기준.** Kotlin coroutine으로 Spot을 구현할 때 이 base class를 상속한다. Java `ZLinkSpot`을
직접 구현하면 `CompletionStage`/`CompletableFuture`를 손으로 다뤄야 한다.

---

## `ZLinkKotlinSpotManager.create` / `getOrCreate`

새 User Spot을 만들거나, 있으면 재사용한다. Java `ZLinkSpotManager`와 같은 semantics를 Kotlin
전용 single-use wrapper로 감싼다.

```kotlin
val created = spotManager.create("room")
    .inMesh("play")
    .request(CreateRoom("ranked"))
    .timeout(Duration.ofSeconds(5))
    .await()

val spotId = created.spot().spotId()
```

**옵션.** `.inMesh(...)`, `.request(...)`, `.timeout(...)`에 더해 terminal `.await()`/`.yield()`
— 각각 Java 레퍼런스의 `create`/`getOrCreate` 항목과 같은 의미다.

**완료 결과.** `ZLinkSpotCreateResult`(Java 타입)를 그대로 반환한다. 같은 option을 두 번 설정하거나
terminal을 두 번 호출하면 `InvalidOperation`이다.

**선택 기준.** Java 레퍼런스의 `create`/`getOrCreate` 항목과 같다. `find`/`close`는 Kotlin 전용
wrapper 없이 Java manager를 그대로 사용한다(단순 조회·종료라 fluent state가 필요 없다).

---

## `sendToSpot` / `requestToSpot` (ZLinkKotlinRouteClient 확장)

Global SpotId 하나로 one-way message를 보내거나 typed request/reply를 주고받는다.
`ZLinkKotlinRouteClient`의 확장 함수로 제공된다.

```kotlin
routeClient.sendToSpot("room-42", PlayerJoinedRoom("player-1")).await()

val reply = routeClient
    .requestToSpot<RoomState>("room-42", GetRoomState())
    .timeout(Duration.ofSeconds(3))
    .await()
```

**옵션.** `ZLinkKotlinSpotSendCall`/`ZLinkKotlinSpotRequestCall<TReply>`가 제공하는 modifier는
Java 레퍼런스 04번의 `sendToSpot`/`requestToSpot`과 같다 — `.metadata(...)`, `.instanceSpot()`/
`.instanceSpot(stableType)`, `.inMesh(...)`, terminal `.await()`(둘 다)/`.yield()`(request만).
Wrapper가 이 fluent state를 유지한 채 terminal에서 Java call을 종료한다.

**완료 결과.** Java 레퍼런스와 같은 완료 kind(`NotFound`/`TypeMismatch`/`DeadlineExceeded` 등).

**선택 기준.** Java 레퍼런스의 `sendToSpot`/`requestToSpot` 선택 기준과 같다.

---

## `publish` (Spot Logical Multicast)

ChannelName과 topic으로 구독자에게 typed event를 발행한다. Java `ZLinkSpotPublisherClient`/
`ZLinkSpotOutbound.publish(...)`를 그대로 사용하며 `ZLinkPublishCall`의 `submit()`을 호출한다 —
Kotlin 전용 coroutine wrapper를 별도로 두지 않는다. 완료 규칙은
[Java 레퍼런스 04번의 `publish`](../../java/reference/04-spot-instance.ko.md) 항목과 같다.

---

## `addTimer` / `runCpuWorker` / `runIoWorker` (Spot 코드 안)

Java `ZLinkSpotContext.addTimer(...)`/`runCpuWorker(...)`/`runIoWorker(...)`를 그대로 사용하되,
timer handler는 `ZLinkSuspendingSpotTimerHandler<TSpot>`(`suspend fun handle(spot, tick)`)로
구현하고, worker 결과는 `ZLinkKotlinWorkerCall<T>`(`suspend fun await()`/`yield()`)로 받는다.

**완료 결과.** Java 레퍼런스 04번의 `addTimer`/`runCpuWorker`/`runIoWorker` 항목과 같다. Relocation
때 logical timer registration이 자동으로 이전되는 규칙도 동일하다.

**선택 기준.** Java 레퍼런스와 같다 — CPU-bound는 `runCpuWorker`, I/O 대기가 있는 작업은
`runIoWorker`를 쓴다.

---

## Handler 등록 (`addHandler<T>()`, Spot 코드 안, `configure()`)

Suspending handler 타입을 등록하는 reified 확장 함수다.

```kotlin
override fun configure() {
    context.handlers().addHandler<StartGameHandler>()
}
```

**옵션.** `ZLinkSpotHandlerRegistry.addHandler<THandler>()`는 내부에서 Java의 raw `Class<?>` 기반
등록으로 위임한다. Handler가 구현하는 interface(`ZLinkSuspendingSpotPacketHandler`,
`ZLinkSuspendingSpotRequestHandler`, `ZLinkSuspendingSpotSubscriptionHandler`,
`ZLinkSuspendingSpotActorSendHandler`, `ZLinkSuspendingSpotActorRequestHandler`)로 실제 역할을
판별한다 — Java 레퍼런스 04번의 handler 등록 표와 대응 관계가 같다.

**완료 결과.** Java 레퍼런스와 같다 — 반환값 없이 동기로 등록되고, 같은 owner의 handler key 중복은
startup 검증에서 드러난다.

**선택 기준.** `configure()`가 호출될 때마다 이 Spot이 처리할 모든 suspending handler를 등록한다.

---

## `leaveActor` / `close` / `destroyActor` / `relocationReady().defer()` (Spot 코드 안)

Java `ZLinkSpotContext`/`ZLinkEntrySpotContext`/`ZLinkInstanceSpotContext`의 같은 이름 메서드를
그대로 호출한다(`suspend`가 아니라 Java `CompletionStage` 반환 — 필요하면 `.await()`로 잇는다).
`relocationReady().defer()`도 Java와 동일하며, `onRelocationReadyCompletedSuspending(...)`이
completion을 받는다. 완료 규칙은
[Java 레퍼런스 04번](../../java/reference/04-spot-instance.ko.md)의 해당 항목과 같다.

---

전체 근거는
[Kotlin Spot exact interface](../../common/spec/server/languages/kotlin/interfaces/spots.ko.md)와
[Java 레퍼런스 04. Spot instance](../../java/reference/04-spot-instance.ko.md)를 참고한다.
