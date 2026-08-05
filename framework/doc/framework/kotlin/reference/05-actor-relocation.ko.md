# 05. Actor relocation

[레퍼런스 목차](README.ko.md)

완료 kind, capacity·timeout 규칙과 relocation 순서는
[Java 레퍼런스 05. Actor relocation](../../java/reference/05-actor-relocation.ko.md)과 완전히
같다. Kotlin이 추가하는 것은 suspending Actor base class와, 같은 fluent state를 coroutine으로
감싼 `ZLinkKotlinActorManager`/`ZLinkKotlinActorClient`다. 정확한 signature는
[Kotlin Actor exact interface](../../common/spec/server/languages/kotlin/interfaces/actors.ko.md)가
소유한다.

---

## `ZLinkSuspendingActor` / `ZLinkSuspendingActorFactory`

Actor lifecycle을 suspend function으로 override하는 abstract base class와 factory다.

```kotlin
class PlayerActor(override val context: ZLinkActorContext) : ZLinkSuspendingActor() {
    override suspend fun onJoinCompletedSuspending(
        completion: ZLinkActorJoinCompletion,
    ) { ... }
}

class PlayerActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        PlayerActor(context)
}
```

**옵션.** `ZLinkSuspendingActor`는 `context: ZLinkActorContext`(property)와
`onJoinCompletedSuspending(completion)`을 override한다. `ZLinkSuspendingActorFactory`는
`createActor(context)`를 override한다.

**완료 결과.** `final override fun context()`/`onJoinCompleted(...)`/`create(...)`가 Java
`CompletionStage` bridge 역할만 하며, 실제 로직은 suspend 메서드가 담당한다.

**선택 기준.** Kotlin coroutine으로 Actor와 factory를 구현할 때 이 base class를 상속한다.

---

## `ZLinkKotlinActorManager.create` / `getOrCreate`

새 Actor를 만들거나, 있으면 재사용한다.

```kotlin
val created = actorManager.create("player-1", "player")
    .inMesh("play")
    .request(SpawnPlayer("player-1"))
    .await()
```

**옵션.** `.inMesh(...)`, `.request(...)`, `.timeout(...)`에 더해 terminal `.await()`/`.yield()`
— Java 레퍼런스의 `create`/`getOrCreate` 항목과 같은 의미다.

**완료 결과.** `ZLinkActorCreateResult`(Java 타입)를 그대로 반환한다. `yield()`는 `SPOT_WIDE`
User Spot·Instance Spot application callback에서만 현재 Spot gate를 반납한다 — 다른 문맥에서는
reservation, factory 실행과 queue 변경 전에 `InvalidOperation`으로 끝낸다.

**선택 기준.** Java 레퍼런스의 `create`/`getOrCreate` 항목과 같다. `find`/`findSpot`/`destroy`는
Kotlin 전용 wrapper 없이 Java manager를 그대로 사용한다.

---

## `sendToActor` / `requestToActor` (ZLinkKotlinActorClient)

Global ActorId 하나로 one-way message를 보내거나 typed request/reply를 주고받는다.

```kotlin
actorClient.sendToActor("player-1", GrantItem("sword")).await()

val reply = actorClient
    .requestToActor<Inventory>("player-1", GetInventory())
    .timeout(Duration.ofSeconds(3))
    .await()
```

**옵션.** `sendToActor`는 `ZLinkKotlinMessageSendCall`(`.metadata(...)`, `.await()`)을,
`requestToActor`는 `ZLinkKotlinRequestCall<TReply>`(`.metadata(...)`, `.timeout(...)`, `.await()`/
`.yield()`)를 반환한다. `requestToActor<TReply>(actorId, request)`는 reified inline extension이다.
Actor send는 one-way `await(): Unit`만 제공하고 `yield()`를 제공하지 않는다.

**완료 결과.** Java 레퍼런스의 `sendToActor`/`requestToActor` 완료 kind와 같다.

**선택 기준.** Reply가 필요 없으면 `sendToActor`, 필요하면 `requestToActor`를 쓴다.

---

## `joinSpot` / `joinEntrySpot` (Actor 코드 안)

현재 Actor를 User Spot 또는 Entry Spot에 참여시킨다. Java `ZLinkActorContext.joinSpot(...)`/
`joinEntrySpot(...)`을 그대로 사용하며, terminal은 Java와 같은 동기 `defer()` 하나뿐이다 —
coroutine terminal을 추가하지 않는다.

```kotlin
context.joinSpot("room-42", JoinRoomRequest("player-1"))
    .timeout(Duration.ofSeconds(5))
    .defer()
```

**완료 결과.** 결과는 `onJoinCompletedSuspending(...)`으로 비동기 전달된다
(`ZLinkActorJoinCompletion`: `Accepted`/`Rejected`/`Failed`). `defer()`는 handler 실행 중 한 번
호출하며 Spot gate나 Actor FIFO claim을 반납하지 않는다.

**선택 기준.** Java 레퍼런스의 `joinSpot`/`joinEntrySpot` 항목과 같다. Entry Spot과 `PER_ACTOR`
User Spot의 Actor에서 호출하면 `InvalidOperation`으로 완료한다.

---

## Relocation 정책 선택 (Actor factory 등록 시점)

`preserveStateWith(AdapterClass::class.java)`로 Java `ZLinkActorRelocationAdapter<TActor>`를
등록한다. Opaque Java `byte[]`는 Kotlin `ByteArray`로 보이며, `capture`/`restore`는 Java와 같은
`CompletionStage`를 반환한다 — Kotlin 전용 suspending adapter는 없다.

| 정책 | 선택 기준 |
| --- | --- |
| `disableRelocation()` | 이 Actor가 다른 node로 옮겨지면 안 될 때 |
| `recreateOnRelocation()` | State 없이 다시 만들어도 되는 Actor일 때 |
| `preserveStateWith(AdapterClass::class.java)` | State를 유지한 채 옮겨야 할 때 |

**완료 결과.** Java 레퍼런스의 relocation 정책 선택 항목과 같은 완료 규칙(capture 결과 최대 64 MiB,
retry-safe 요구 등)을 따른다. Adapter 대상 type이 factory의 Actor type과 다르면 socket bind 전
configuration error다.

**선택 기준.** Java 레퍼런스와 같다 — factory 등록 시점에 한 번만 정한다. Kotlin은 reified helper나
policy를 생략하는 overload를 추가하지 않는다.

---

전체 근거는
[Kotlin Actor exact interface](../../common/spec/server/languages/kotlin/interfaces/actors.ko.md)와
[Java 레퍼런스 05. Actor relocation](../../java/reference/05-actor-relocation.ko.md)을 참고한다.
