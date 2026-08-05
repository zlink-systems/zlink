# 06. Stream session

[레퍼런스 목차](README.ko.md)

Session bind 실패 규칙, physical disconnect 처리와 relocation route 갱신은
[Java 레퍼런스 06. Stream session](../../java/reference/06-stream-session.ko.md)과 완전히 같다.
Kotlin이 추가하는 것은 suspending session base class와, one-way call을 감싼 Kotlin 전용 wrapper다.
정확한 signature는
[Kotlin STREAM session exact interface](../../common/spec/server/languages/kotlin/interfaces/stream-session.ko.md)가
소유한다.

---

## `ZLinkSuspendingSession`

Session lifecycle을 suspend function으로 override하는 abstract base class다.

```kotlin
class GameSession(private val ctx: ZLinkSessionContext) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = ctx

    override suspend fun onConnectedSuspending() { ... }
    override suspend fun onDisconnectedSuspending() { ... }
    override suspend fun onErrorSuspending(error: ZLinkStreamError) { ... }
}
```

**옵션.** Override 가능한 suspend 메서드는 `onConnectedSuspending`, `onDisconnectedSuspending`,
`onErrorSuspending(error)`, `onDispatchSuspending(dispatch, payload)`(fallback dispatch)다. Typed
packet은 별도 `ZLinkSuspendingTypedSessionPacketHandler<TSessionContext, TMessage>`
(`packetName()`, `messageType()`, `suspend fun handle(context, dispatch, message)`)로 구현해
`addSessionPacketHandler(...)`(topology-discovery category)로 등록한다.

**완료 결과.** `final override fun onConnected/onDisconnected/onError/onDispatch`가 Java
`CompletionStage` bridge 역할만 한다. 완료 순서와 handshake 실패 처리는 Java 레퍼런스 06번과 같다.

**선택 기준.** Kotlin coroutine으로 Session을 구현할 때 이 base class를 상속한다.

---

## `send` / `reply` (ZLinkKotlinSessionClient)

연결된 client에 one-way message를 보내거나 현재 request에 응답한다.

```kotlin
sessionContext.client().send(ServerTick(tickNumber)).await()
sessionContext.client().reply(GetPlayerStateResult(state)).await()
```

**옵션.** `ZLinkKotlinSessionSendCall`은 `.metadata(...)`, `.compress()`, `.await()`를,
`ZLinkKotlinSessionReplyCall`은 `.compress()`, `.await()`를 제공한다(Java와 마찬가지로 `reply`에는
metadata modifier가 없다).

**완료 결과.** Java 레퍼런스의 `send`/`reply` 완료 kind와 같다. Application은 `await(): Unit`으로
local STREAM queue admission만 기다리며 Java `CompletionStage`와 submission result type을 직접
쓰지 않는다.

**선택 기준.** Java 레퍼런스의 `send`(Session 코드 안)/`reply` 항목과 같다.

---

## `bindOrGetActor` (ZLinkSessionActors 확장)

이 STREAM session에 Actor를 묶는다. Java `bindOrGet(actorRef)`를 감싼 suspend 확장 함수다.

```kotlin
val bound = sessionContext.actors().bindOrGetActor(actorRef)
```

**옵션.** 이 함수에는 modifier가 없다 — `ActorRef`만 받는다.

**완료 결과.** Java 레퍼런스의 `bind`/`bindOrGet` 완료 kind(`NotFound`/`InvalidOperation`/
`Unavailable`)와 같다.

**선택 기준.** Java 레퍼런스와 같다 — Actor가 이 client 연결로 직접 push해야 할 때 bind한다.

---

## `relay` (ZLinkKotlinSessionActor)

Bind로 얻은 handle을 통해 이 Actor 쪽에서 client로 payload를 전달한다.

```kotlin
sessionActor.relay(ZLinkMessage.of(RoomUpdated(state))).await()
```

**옵션.** `ZLinkKotlinSubmissionCall`(`.await()`만)을 반환한다. Payload만 받는 overload와
`ZLinkSessionDispatchContext`를 함께 받는 overload가 있다 — 후자는 explicit reply capability를
runtime에 이전한다(Java 레퍼런스 06번의 `relay` 항목 참고). `notifyDisconnected()`는 Kotlin
wrapper 없이 Java `ZLinkSessionActor.notifyDisconnected()`를 그대로 사용한다.

**완료 결과.** Java 레퍼런스와 같다.

**선택 기준.** Java 레퍼런스의 `relay`/`notifyDisconnected` 항목과 같다.

---

## `send` (ZLinkKotlinBoundSession, Actor 코드 안)

Actor에서 자신에게 bind된 client로 one-way message를 보낸다.

```kotlin
context.boundSession().send(InventoryChanged(item)).await()
```

**옵션.** `ZLinkKotlinMessageSendCall`(`.metadata(...)`, `.await()`)을 반환한다. 연결을 끊는
`disconnect()`는 Kotlin wrapper 없이 Java `ZLinkBoundSession.disconnect()`를 그대로 사용한다.

**완료 결과·선택 기준.** Java 레퍼런스의 `send`(bound session)/`disconnect` 항목과 같다.

---

## `close` (연결 종료)

Java `ZLinkSessionContext.close()`를 그대로 호출한다(필요하면 `.await()`로 잇는다). 완료 규칙은
[Java 레퍼런스 06번의 `close`](../../java/reference/06-stream-session.ko.md) 항목과 같다.

---

전체 근거는
[Kotlin STREAM session exact interface](../../common/spec/server/languages/kotlin/interfaces/stream-session.ko.md)와
[Java 레퍼런스 06. Stream session](../../java/reference/06-stream-session.ko.md)을 참고한다.
