# 06. Stream session

[레퍼런스 목차](README.ko.md)

이 category는 STREAM session 코드 안에서 쓰는 진입점(`ZLinkSession`, `ZLinkSessionClient`,
`ZLinkSessionActors`, `ZLinkSessionActor`)과 Actor 코드 안에서 bound session에 쓰는 진입점
(`ZLinkBoundSession`)을 다룬다. 정확한 signature는
[Java STREAM session exact interface](../../common/spec/server/languages/java/interfaces/stream-session.ko.md)와
[Java Actor exact interface](../../common/spec/server/languages/java/interfaces/actors.ko.md)가
소유한다.

---

## Session 콜백 구현 (`ZLinkSession`)

이 STREAM session이 받을 lifecycle 이벤트와 typed packet을 처리한다. `registerSession(...)`
(topology-discovery category)로 등록한 타입이 구현한다.

```java
public class GameSession implements ZLinkSession {
    @Override
    public ZLinkSessionContext context() { return context; }

    @Override
    public CompletionStage<Void> onConnected() { ... }

    @Override
    public CompletionStage<Void> onDisconnected() { ... }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) { ... }
}
```

**옵션.** Typed packet은 session class 자체에 두지 않고, 별도 handler 타입을
`ZLinkTypedSessionPacketHandler<TSessionContext, TMessage>`로 구현해
`addSessionPacketHandler(...)`(topology-discovery category)로 등록한다.

| Handler interface | 등록 방식 |
| --- | --- |
| `ZLinkTypedSessionPacketHandler<TSessionContext, TMessage>` | `messageType()`이 처리할 타입을 선언하고, `handle(context, dispatchContext, message)`를 구현. `addSessionPacketHandler(handlerType)`으로 등록 |
| `ZLinkSession.onDispatch(dispatchContext, message)` (default method) | 위 typed handler가 처리하지 못한 packet에 대한 fallback. Override는 선택적이다 |

**완료 결과.** 모든 callback은 `CompletionStage<Void>`를 반환한다. `onConnected`/`onDisconnected`는
연결·해제마다 한 번, `onError`는 transport 오류마다, typed handler는 Framework 내부 recv loop가
header framing과 queue admission을 끝낸 뒤 packet마다 호출한다. Handshake 실패는 session이
만들어지기 전이므로 `onError`가 아니라 runtime monitoring에만 기록된다.

**선택 기준.** `stream-session` topology를 쓰는 모든 host가 구현한다.
`ZLinkSessionDispatchContext.canReply()`가 `true`인 packet에만 `reply`로 응답할 수 있다.

---

## `send` (ZLinkSessionClient)

연결된 client에 one-way message를 보낸다. `ZLinkSessionContext.client()`가 반환하는
`ZLinkSessionClient`로 호출한다.

```java
sessionContext.client().send(new ServerTick(tickNumber)).submit();
```

**옵션.** `ZLinkSessionSendCall`이 제공하는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | client에 전달할 key-value |
| `.compress()` | 비압축 | 등록된 stream compression codec으로 payload를 압축 |
| `.submit()` | 필수 terminal | source-local admission까지만 기다린다 |

**완료 결과.** messaging-execution category의 one-way 완료 kind와 같다 — socket send timeout까지
기다린 뒤 없으면 `DEADLINE_EXCEEDED`, connection 단절은 `UNAVAILABLE`인 `ZLinkFrameworkException`으로
완료한다.

**선택 기준.** Client가 보낸 request가 아닌, server가 먼저 보내는 push 메시지에 쓴다. Client의
request에 답할 때는 `reply`를 쓴다.

---

## `reply` (ZLinkSessionClient)

현재 처리 중인 request packet에 응답한다.

```java
sessionContext.client().reply(new GetPlayerStateResult(state)).submit();
```

**옵션.** `ZLinkSessionReplyCall`이 제공하는 modifier는 다음과 같다 — `.compress()`와 필수 terminal
`.submit()`. `send`와 달리 metadata modifier가 없다.

**완료 결과.** 이 request의 one-shot reply token을 원자적으로 claim한 뒤 전송한다. 같은 token으로
만든 두 번째 `reply` 호출은 claim에 실패해 transport를 시도하지 않고 exceptional completion으로
끝난다. Caller의 request timeout은 wire로 전달되지 않으므로 이 reply의 admission deadline은
STREAM socket send timeout만 사용한다.

**선택 기준.** `ZLinkSessionDispatchContext.canReply()`가 `true`인 packet(request)에만 쓴다.
Client가 보낸 것이 아닌 새 메시지를 보내려면 `send`를 쓴다.

---

## `bind` / `bindOrGet` (ZLinkSessionActors)

이 STREAM session에 Actor를 묶어 Actor 쪽에서 이 연결로 push할 수 있게 한다.
`ZLinkSessionContext.actors()`로 호출한다.

```java
ZLinkSessionActor bound = sessionContext.actors().bindOrGet(actorRef)
    .toCompletableFuture().get();
```

**옵션.** 이 호출에는 modifier가 없다 — `ActorRef`만 받는다.

**완료 결과.** `bind`는 매번 새 binding을 만든다. `bindOrGet`은 이미 bound된 같은 incarnation이
있으면 그것을 반환한다. Binding은 `actorId + objectGeneration`의 exact incarnation 하나로
고정된다. Active Message Follow route가 없으면 `NOT_FOUND`, generation이 다르면
`INVALID_OPERATION`, pre-commit seal 중이면 `UNAVAILABLE`이다. `find(actorId)`로 이미 bound된
handle을 동기 조회할 수 있고, `bound()`는 현재 session에 bound된 전체 목록을 반환한다.

**선택 기준.** Actor가 이 client 연결로 직접 push해야 할 때 bind한다. Relocation이 일어나도
`ZLinkSessionActor.ref()`가 current location snapshot으로 갱신되므로 application이 다시 bind할
필요는 없다.

---

## `relay` / `notifyDisconnected` (ZLinkSessionActor)

Bind로 얻은 `ZLinkSessionActor`를 통해 이 Actor 쪽에서 client로 payload를 전달하거나 연결 단절을
통지한다.

```java
sessionActor.relay(ZLinkMessage.of(new RoomUpdated(state)))
    .toCompletableFuture().get();
```

**옵션.** 두 호출 모두 modifier가 없다 — payload(`relay`)만 받는다. `relay`는
`ZLinkSessionDispatchContext`를 함께 받는 overload도 있다.

**완료 결과.** Payload만 받는 `relay`는 source-local admission을 수락하면 정상 완료하는 one-way
operation이다. Dispatch context를 받는 overload는 explicit current STREAM request reply
capability를 즉시 runtime에 이전한다 — submit되면 typed reply가 original correlation을
terminal-once로 완료하고, admission 실패면 같은 correlation을 typed failure로 완료한다.
`notifyDisconnected`는 connection이 유지된 상태에서 논리적 단절을 알리는 notification이며 callback
terminal까지 기다린다. Physical disconnect는 Framework가 자동으로 현재 binding 전체에 통지하므로
이 호출이 그 대체 경로는 아니다.

**선택 기준.** Actor 쪽 코드에서 특정 bound client에 직접 전달할 때 쓴다. Request에 대한 응답은
Session 쪽 `reply`가 처리한다.

---

## `send` (ZLinkBoundSession, Actor 코드 안)

Actor에서 자신에게 bind된 client로 one-way message를 보낸다. `ZLinkActorContext.boundSession()`이
반환하는 `ZLinkBoundSession`으로 호출한다.

```java
context.boundSession().send(new InventoryChanged(item)).submit();
```

**옵션.** `ZLinkBoundSessionSendCall`이 제공하는 `.metadata(...)`와 필수 terminal `.submit()`이
있다.

**완료 결과.** messaging-execution category의 one-way 완료 kind와 같다. 이 표면은 client를 향한
새 request operation을 제공하지 않는다 — client request에 대한 reply는 Actor request handler의
반환값으로 처리한다.

**선택 기준.** Actor 코드 쪽에서 bound client로 push할 때 쓴다. Session 쪽에서 직접 보내려면 위
`send`(ZLinkSessionClient) 항목을 쓴다. 연결을 끊으려면 `ZLinkBoundSession.disconnect()`를 쓴다.

---

## `close` (연결 종료)

Session을 닫는다. `ZLinkSessionContext.close()`가 제공한다.

```java
sessionContext.close().toCompletableFuture().get();
```

**옵션.** 이 호출에는 modifier가 없다.

**완료 결과.** 연결을 닫는다. Remote unbind completion을 bounded lifecycle deadline 안에서
관찰하며, timeout이나 terminal failure는 close failure로 반환한다 — 성공·실패와 관계없이 local
binding과 session transport는 정리한다.

**선택 기준.** Application이 자발적으로 이 STREAM 연결을 끊어야 할 때 쓴다. Actor 쪽에서 bound
client 연결을 끊으려면 `ZLinkBoundSession.disconnect()`를 쓴다.

---

## `disconnect` (Actor 코드 안, bound session)

Actor에서 자신에게 bind된 client 연결을 끊는다. `ZLinkBoundSession.disconnect()`로 호출한다.

```java
context.boundSession().disconnect().toCompletableFuture().get();
```

**옵션.** 이 호출에는 modifier가 없다.

**완료 결과.** Bound session과의 연결을 끊는다.

**선택 기준.** Actor 쪽 코드에서 특정 client 연결을 더 유지할 필요가 없을 때 쓴다. Session 쪽에서
직접 끊으려면 `close` 항목을 쓴다.

---

전체 근거는
[Java STREAM session exact interface](../../common/spec/server/languages/java/interfaces/stream-session.ko.md)와
[Java Actor exact interface](../../common/spec/server/languages/java/interfaces/actors.ko.md)를
참고한다.
