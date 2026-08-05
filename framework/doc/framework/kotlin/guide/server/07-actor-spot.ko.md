---
title: "7. Actor와 Spot · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/07-actor-spot.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 6. Spot](06-spot.ko.md) | [다음: 8. Session과 Actor binding](08-actor-session.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C#/.NET](../../../dotnet/guide/server/07-actor-spot.ko.md) · [C++](../../../cpp/guide/server/07-actor-spot.ko.md) · [Java](../../../java/guide/server/07-actor-spot.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/07-actor-spot.ko.md)
<!-- language-switch:end -->

# 7. Actor와 Spot

> **이 장의 계약 소유 문서** — [Actor 모델](../../../common/spec/14-actor-model.ko.md)과
> [Spot과 Actor membership](../../../common/spec/15-spot-actor.ko.md)이 동작을,
> [언어별 Actor · Spot 공개 계약](../../../common/spec/server/languages/README.ko.md)이 정확한
> 시그니처를 소유한다.

Actor는 전역 문자열 `ActorId`로 찾는 상태 객체다. 생성 직후에는 Object Server의 Entry Spot에
존재한다. Application handler가 join을 예약하면 User Spot으로 이동한다.

Actor 위치와 client session binding은 서로 다른 상태다. Client가 연결되지 않아도 Actor와 Spot
membership은 유지된다. Session binding은 [다음 문서](08-actor-session.ko.md)에서 설명한다.

## 1. 등록

Object Server에 Entry Spot과 Actor factory를 함께 등록한다. `actorType`을 등록한 `Serving` node가
생성 후보가 된다.

아래는 [Bingo 샘플](../../../common/sample/bingo/README.ko.md)의 Play 서버 등록이다.

```kotlin
mesh.objects().server()
    .addEntrySpot(BingoEntrySpot::class.java)
    .addActorFactory(
        SampleNames.PlayerActorType,
        PlayerActor::class.java,
        PlayerActorFactory::class.java,
    ) { factory -> factory.preserveStateWith(PlayerActorRelocationAdapter::class.java) }
```

Relocation policy는 factory 등록에서 하나를 고정하며 실행 중에 변경하지 않는다.
Actor가 다른 node의 Spot으로 join할 때와 host `relocate`로 이전할 때 모두 이 policy를
따른다.

| policy | 다른 node에서 만드는 방법 |
| --- | --- |
| `DisableRelocation()` | Cross-node 이동을 시작하기 전에 거절한다. 이 대상이 남아 있으면 host relocation을 완료할 수 없다. |
| `RecreateOnRelocation()` | 같은 logical identity로 새 instance를 만든다. 대기 중이던 message와 timer는 유지하지만 application state는 복원하지 않는다. |
| `PreserveStateWith<TAdapter>()` | adapter가 저장한 `byte[]`를 새 instance에 복원한다. Framework queue와 timer도 함께 유지한다. |

## 2. Actor 만들기

`create`는 같은 ActorId가 이미 있으면 실패한다. `getOrCreate`는 같은 type의 Ready Actor가 있으면
`Existing`을 반환한다. Caller는 target node를 지정하지 않는다.

```kotlin
val result = actors
    .getOrCreate(playerId, "player")
    .inMesh("play")
    .request(CreatePlayer(displayName))
    .timeout(Duration.ofSeconds(10))
    .submit()
    .await()

val actor = when (result) {
    is ZLinkActorCreateResult.Existing -> result.actor()
    is ZLinkActorCreateResult.Created -> result.actor()
    else -> error("Player creation was rejected.")
}
```

`ActorRef`는 exact incarnation과 조회 당시 owner route를 담는다. Session binding이나 exact destroy에
사용한다. 일반 Actor 메시징은 ActorId만 사용한다.

```kotlin
// Java 표면이 Optional을 돌려주므로 Kotlin에서는 orElse(null)로 받는다.
val current = actors.find(playerId).await().orElse(null)
val currentSpot = actors.findSpot(playerId).await().orElse(null)

if (current != null) {
    // generation이 다른 Actor는 종료하지 않는다.
    actors.destroy(current).await()
}
```

Actor는 Entry Spot에서만 종료할 수 있다. User Spot에 있으면 먼저 Entry Spot join을 완료한다.

## 3. Entry Spot

Entry Spot은 Actor 생성 요청을 승인하거나 거절하고, Actor가 들어오고 나가는 lifecycle을 처리한다.

**membership callback**은 Actor가 이 Spot의 구성원이 되거나 빠질 때 Framework가 호출하는
lifecycle callback을 말한다. Entry Spot에는 넷이 있다.

| Callback | 언제 호출되나 |
| --- | --- |
| `onCreateActor` | 새 Actor가 이 Entry Spot을 최초 membership으로 삼을 때. 승인·거절을 결정한다 |
| `onJoinedActor` | 다른 Spot에 있던 Actor가 이 Entry Spot으로 들어온 commit이 끝났을 때 |
| `onLeaveActor` | 이 Entry Spot에 있던 Actor가 다른 Spot으로 빠져나간 commit이 끝났을 때 |
| `onDisconnectActor` | 이 Entry Spot 소속 Actor의 client 연결이 끊겼을 때 |

Relocation으로 Actor가 다른 node의 Entry Spot에 복원되는 경우에는 이 callback들을 호출하지
않는다. Relocation은 membership을 그대로 유지한 채 실행 위치만 옮기는 것이므로, application이
보기에 "들어오거나 나간" 사건이 아니기 때문이다.

> **샘플에서 보기 — [TicTacToe](../../../common/sample/tictactoe/README.ko.md).** player가
> 처음 들어오는 Entry Spot이다. 저장소의 실제 코드다.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/entryspot/PlayEntrySpot.kt:doc-entry-spot"
```

최소 형태로 보면 이렇다.

```kotlin
// <PlayerActor> — 이 Entry Spot이 membership을 관리할 Actor type이다.
class PlayEntrySpot(private val entryContext: ZLinkEntrySpotContext) : ZLinkEntrySpot<PlayerActor> {

    override fun context(): ZLinkEntrySpotContext = entryContext

    // Spot instance가 준비될 때 한 번 호출된다.
    override fun configure() {
        // JoinGameHandler가 PlayerActor 앞으로 온 JoinGame packet을 받는다.
        // 어떤 종류인지는 handler에 붙인 @ZLinkSpotActorSend가 정한다.
        entryContext.handlers().addHandler(JoinGameHandler::class.java)
    }

    // 새 Actor가 이 Entry Spot을 최초 membership으로 삼을 때 호출된다.
    // 반환값이 이 Actor를 만들지 말지를 결정한다 — 이 Spot이 admission 관문이다.
    override suspend fun onCreateActor(
        actor: PlayerActor, createRequest: ZLinkMessage): ZLinkActorCreateResponse {
        actor.setDisplayName(createRequest.decode(CreatePlayer::class.java).displayName)
        return ZLinkActorCreateResponse.accept()
    }

    // User Spot에 있던 Actor가 돌아온 commit이 끝난 뒤 호출된다.
    override suspend fun onJoinedActor(actor: PlayerActor) {}

    // Actor가 User Spot으로 빠져나간 commit 뒤 호출된다.
    override suspend fun onLeaveActor(actor: PlayerActor) {}
}
```

Entry Spot은 Actor별 application state를 따로 보관하지 않는 편이 안전하다. Actor state는 Actor가
소유하고, Entry Spot은 handler와 membership lifecycle만 제공한다.

Actor를 종료하려면 먼저 Entry Spot으로 돌아온 뒤 현재 Actor instance를
entry spot context의 actor 파기 호출에 넘긴다.

```kotlin
// Entry Spot이 현재 Actor의 종료를 요청한다.
entryContext.destroyActor(actor).await()
```

이 호출은 membership lifecycle callback을 다시 호출하지 않고 native actor ref, Framework registry와
bound session mapping을 정리한다. User Spot에 있는 Actor는 직접 종료할 수 없다. 먼저 leave를 완료해
Entry Spot으로 돌아와야 한다.

## 4. User Spot membership

User Spot은 join 요청을 먼저 승인하거나 거절한다. 승인 뒤 membership이 commit되면
`onJoinedActor`가 호출된다.

```kotlin
class GameRoom(private val spotContext: ZLinkSpotContext) : ZLinkSpot<PlayerActor> {

    override fun context(): ZLinkSpotContext = spotContext

    override suspend fun onActorJoin(
        actorId: String, request: ZLinkMessage): ZLinkSpotActorJoinResult {
        val join = request.decode(JoinGame::class.java)
        return if (hasSeat(join.seat)) ZLinkSpotActorJoinResult.accept(Joined(join.seat))
               else ZLinkSpotActorJoinResult.reject(RoomFull())
    }

    override suspend fun onJoinedActor(actor: PlayerActor) {}
    override suspend fun onLeaveActor(actor: PlayerActor) {}
}
```

## 5. Join 실행 시점

### `defer()`가 하는 일

`defer()`는 join을 **지금 실행하지 않고 현재 handler에 예약**한다. 호출 시점에 Framework는 세
가지를 고정한다 — join request의 immutable snapshot, `timeout(...)`으로 계산한 absolute
deadline, 그리고 이 handler가 끝난 뒤 실행할 barrier다.

예약한 barrier의 운명은 handler의 종료 방식이 정한다.

| handler 종료 | 예약한 join |
| --- | --- |
| 정상 종료 | 활성화되어 실행을 시작한다 |
| 예외·cancellation·reply encoding 실패 | 폐기한다. join은 시작되지 않는다 |

`defer()`는 **현재 handler의 registration scope가 열려 있는 동안에만** 호출할 수 있다. handler가
끝난 뒤나 handler에서 떼어낸 background task에서 호출하면 `InvalidOperation`이다.

**부를 수 있는 자리는 정해져 있다.**

| 부를 수 있다 | 부를 수 없다 |
| --- | --- |
| Actor send · request handler | factory와 구성 단계 |
| User · Entry Spot의 packet · request · subscription · timer handler | lifecycle callback |
| | relocation adapter |
| | Instance Spot handler |
| | handler에서 떼어낸 background task |

오른쪽에서 부르면 `InvalidOperation`이다. **떼어낸 task는 Framework가 모든 언어에서
잡아낸다고 보장하지 않는다** — handler가 끝나기 전에 발견되지 않을 수 있으므로 애초에 그
자리에서 부르지 않는다.

같은 call에서 `defer()`를 두 번 부르면 `InvalidOperation`이고, 그 Actor에 이미 다른
membership 전환이 걸려 있으면 `Unavailable`이다. **이미 그 Spot에 속해 있는 Actor가 같은
Spot에 join하면** 위치를 바꾸지 않고 성공으로 끝난다 — Store도 membership도 건드리지 않고
join · joined · leave callback도 실행하지 않는다.

### `joinSpot`이 `defer()`로만 실행되는 이유

join call에는 `Async`가 없다. 결과를 그 자리에서 기다리는 형태를 제공하지 않는 이유는 join이
하는 일 때문이다.

- **join은 이 Actor의 위치와 membership을 바꾼다.** target Spot의 owner가 다른 node면 같은
  operation 안에서 Actor relocation까지 수행한다 — 위치 조회, target admission callback, Store
  commit이 모두 포함된다.
- **그 완료를 현재 turn 안에서 기다리면 자기 자신을 막는다.** Actor는 자기 queue의 작업을 한
  번에 하나씩 실행한다. 지금 실행 중인 handler가 join 완료를 기다리면, 그 join이 끝나기 위해
  필요한 이 Actor의 후속 처리(membership commit 뒤의 lifecycle callback)가 같은 queue에서 대기하게
  된다.
- **완료 시점의 실행 주체가 바뀔 수 있다.** cross-node join이 성공하면 `Accepted` callback을 받는
  것은 **target node의 Actor**다. 현재 handler가 있는 source Actor는 그 시점에 이미 정리 중이므로,
  "이 handler 안에서 결과를 받는다"는 형태 자체가 성립하지 않는다.

그래서 계약은 **등록과 실행을 분리**한다. handler는 join을 예약하고 정상 종료하며, Framework는
그 뒤에 위치 조회와 Store 작업을 시작한다. 결과는 아래 completion callback으로 온다. 이
분리 덕분에 현재 Actor job과 join 완료 callback의 실행 순서가 섞이지 않는다.

barrier가 활성화된 뒤에는 그 뒤에 도착한 일반 message가 completion callback보다 먼저 실행되지
않는다. join이 끝나기 전까지 그 Actor의 일반 처리는 대기한다.

### 등록과 결과 수신

Actor handler에서 join을 예약한다. handler는 member Actor 앞 one-way packet을 받는 별도
class이며([06-spot §4.1](06-spot.ko.md#41-handler-종류와-구현할-interface)), 구성 단계에서
actor packet으로 등록한 것이다.
`defer()` 뒤에는 이 handler를 정상 종료시키는 것 외에 할 일이 없다.

> **샘플에서 보기 — [TicTacToe](../../../common/sample/tictactoe/README.ko.md).** player가
> 방에 들어가겠다고 예약하는 handler다. 저장소의 실제 코드다.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/entryspot/handlers/PlayActorJoinGameHandler.kt:doc-join-defer"
```

최소 형태로 보면 이렇다.

```kotlin
class JoinGameHandler : ZLinkSpotActorSendHandler<PlayEntrySpot, PlayerActor, JoinGame> {

    override suspend fun handle(
        entrySpot: PlayEntrySpot,  // 이 Actor가 지금 속한 Spot이다.
        actor: PlayerActor,        // join을 요청한 Actor다.
        messageContext: ZLinkMessageContext,
        command: JoinGame,
    ) {
        actor.context()
            .joinSpot(command.spotId, JoinGameRequest(command.seat))
            .timeout(Duration.ofSeconds(5))
            .defer() // 현재 handler가 성공한 뒤 join을 시작한다.
    }
}
```

결과는 Actor의 `onJoinCompleted`로 받는다. 어느 Actor가 이 callback을 실행하는지는 결과에
따라 다르다 — `Accepted`는 위치 변경을 commit한 **target** Actor가, `Rejected`와 commit 전
`Failed`는 기존 **source** Actor가 받는다.

```kotlin
override suspend fun onJoinCompleted(completion: ZLinkActorJoinCompletion) {
    when {
        // 위치와 membership 변경이 commit됐다. completion.actor()가 현재 ActorRef다.
        completion is ZLinkActorJoinCompletion.Accepted ->
            rememberCurrentLocation(completion.actor())
        // target의 admission callback이 join을 거절했다. 위치는 그대로다.
        completion is ZLinkActorJoinCompletion.Rejected ->
            clearPendingJoin()
        // 오류 종류만 받는다. 재시도 여부는 업무 상태와 idempotency를 확인해 결정한다.
        completion is ZLinkActorJoinCompletion.Failed ->
            handleJoinFailure(completion.kind())
    }
}
```

User Spot에서 Entry Spot으로 돌아갈 때도 같은 방식이다.

```kotlin
actor.context()
    .joinEntrySpot(LeaveGame(reason))
    .timeout(Duration.ofSeconds(5))
    .defer()
```

`operationId`는 이 completion이 재시도된 결과인지 구분하는 idempotency ID다. 같은 `operationId`의
callback이 다시 실행되어도 안전하도록 처리한다.

### 등록 한도

한 handler가 예약할 수 있는 양에 상한이 있다.

| 무엇 | 상한 |
| --- | --- |
| 한 handler의 join 예약 수 | 64개 |
| join request 하나의 인코딩 크기 | 1 MiB |
| 한 handler가 예약한 request 크기 합계 | 8 MiB |
| cross-node join의 application reply | 1 MiB |
| timeout 기본값 | 5초. 지정하면 유한한 양수여야 한다 |

**상한을 넘기면 그 자리에서 오류로 끝난다.** 일부만 등록되고 나머지가 빠지는 상태는
만들지 않는다. request와 reply 상한은 서로 독립이라 하나로 합쳐 계산하지 않는다.

### 예약한 Actor에 request를 보내면 안 된다

`defer()`로 barrier를 건 Actor에게 **같은 handler에서 request를 보내고 reply를 기다리면
순환 대기**가 된다. request는 barrier 뒤에서 기다리고, barrier는 이 handler가 끝나야
열리는데, handler는 reply를 기다리느라 끝나지 못한다.

Framework가 이 request를 **제출하기 전에 `InvalidOperation`으로 거부한다.** 멈추지 않고
오류로 끝나므로, 이 오류를 보면 예약과 request의 대상이 같은 Actor인지 확인한다.

### 예약이 살아남지 못하는 경우

예약과 barrier는 **현재 process 메모리에만 있다.** join이 실행되거나 Store에 반영되기 전에
process가 내려가면 그 예약은 재생되지 않는다. Actor의 위치와 membership은 원래 상태
그대로다 — 절반만 옮겨진 상태로 남지 않는다.

`relocate`나 `shutdown`과 겹치면 **먼저 확정된 쪽을 따른다.** join이 먼저 자리를 잡았으면
maintenance가 join이 끝날 때까지 기다리고, relocation seal이 먼저면 join이 `Unavailable`로,
shutdown seal이 먼저면 `ShuttingDown`으로 끝난다.

## 6. Actor 메시징

Actor가 어느 Spot과 node에 있는지 몰라도 ActorId로 메시지를 보낼 수 있다.

```kotlin
actorClient.sendToActor(playerId, AwardExperience(10)).submit().await()

val profile = actorClient
    .requestToActor(playerId, GetPlayerProfile())
    .timeout(Duration.ofSeconds(3))
    .submit(PlayerProfile::class.java)
    .await()
```

Actor가 다른 node로 옮겨가는 중에도 caller는 ActorId만 지정한다. Framework는 호출할 때마다
Location Store에 기록된 **현재 owner**를 다시 조회해 그 node로 보낸다.

옮겨가기 직전의 위치를 캐시하고 있던 caller가 이전 owner로 보낸 message도 버려지지 않는다.
그 message를 받은 이전 owner node가 새 owner에게 **대신 전달**한다. 이것을 Message Follow라
한다 — 보내는 쪽에 새 주소를 알려 주고 다시 보내게 하는 redirect가 아니라, 받은 node가 넘겨주는
방식이다. 이 전달은 Message Follow duration 안에서만 유효하며, 그 뒤에 도착한 message는 일반
stale route 실패로 처리한다. Application은 `NodeRid`를 추적하지 않는다.

**이동 중에 보낸 request도 원래 caller에서 완료된다.** Target이 처리한 reply는 원래 caller로
correlate되고, timeout은 caller의 기존 경로를 그대로 따르며, 늦게 도착한 reply는 drop된다
([spot-actor 스펙 §10.5](../../../common/spec/15-spot-actor.ko.md)). 이동 중 reply를 기다리는
request 수는 `zlink.mesh_node.requests.inflight`의 `surface=actor` 값으로 관측한다
([12-operations](12-operations.ko.md#1-런타임-메트릭)).

## 7. Relocation state adapter

Adapter는 Actor instance의 application state만 byte 배열로 저장하고 복원한다. Location authority,
queue, timer, accepted journal과 session route는 Framework가 처리한다.

> **샘플에서 보기 — [TicTacToe](../../../common/sample/tictactoe/README.ko.md).** player
> Actor의 상태를 담고 푸는 adapter다. 저장소의 실제 코드다.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/actors/PlayActorRelocationAdapter.kt:doc-relocation-adapter"
```

최소 형태로 보면 이렇다.

```kotlin
class PlayerActorRelocationAdapter : ZLinkActorRelocationAdapter<PlayerActor> {

    override suspend fun capture(actor: PlayerActor): ByteArray = actor.exportState()

    override suspend fun restore(actor: PlayerActor, payload: ByteArray) {
        actor.importState(payload)
    }
}
```

Capture와 restore는 같은 relocation에서 다시 호출될 수 있다. Adapter는 retry-safe해야 하며,
payload memory를 callback 밖에서 보관하려면 복사해야 한다.

## 8. 관련 문서

- 이 챕터 계약의 실행 검증 예문: [13. Interface 카탈로그](13-interface-catalog.ko.md) §4 — 검증 클래스 `ActorContracts`
- Session과 Actor binding: [Session Actor Dispatch](08-actor-session.ko.md)
- STREAM server와 client: [STREAM](09-stream.ko.md)
- Actor·Spot 주소 해석 규칙: [Object routing](../../../common/spec/18-object-routing.ko.md)
