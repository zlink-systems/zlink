# 03. Messaging execution — Channel messaging

[레퍼런스 목차](README.ko.md)

완료 kind, timeout 규칙, codec 등록은
[Java 레퍼런스 03. Messaging execution](../../java/reference/03-messaging-execution.ko.md)과
완전히 같다 — Kotlin은 같은 operation을 coroutine 모양으로 감싼 `ZLinkKotlinClient`/
`ZLinkKotlinRouteClient`/`ZLinkKotlinFanoutClient`만 추가한다. 정확한 signature는
[Kotlin channel messaging exact interface](../../common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md)가
소유한다.

Kotlin application은 Java `ZLinkRouteClient`/`ZLinkFanoutClient`를 직접 쓰지 않는다 — 이 Kotlin
전용 client와 call wrapper가 Java call을 내부에 보관하며 일반 완료는 `await()`, 현재 Spot turn을
반납하는 완료는 `yield()`로 투영한다.

---

## `sendToChannel` / `sendToNode` (ZLinkKotlinMessageSendCall)

One-way message를 보낸다. `ZLinkKotlinClient.sendToChannel(...)`과
`ZLinkKotlinRouteClient.sendToChannel(...)`/`sendToNode(...)`가 같은 반환 타입
`ZLinkKotlinMessageSendCall`을 쓴다.

```kotlin
routeClient.sendToChannel("game.api", PlayerOnline("player-1")).await()
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | handler에 전달할 key-value |
| `.await()` | 필수 terminal, `suspend fun` | source-local admission 성공까지만 기다린다. 정상 결과는 `Unit` |

**완료 결과.** Java 레퍼런스의 `sendToChannel`/`sendToNode` 완료 kind와 같다 — 실패는 Java stage의
exception을 그대로 전달한다.

**선택 기준.** Reply가 필요 없는 fire-and-forget에 쓴다. Reply가 필요하면
`requestToChannel`/`requestToNode`를 쓴다.

---

## `requestToChannel` / `requestToNode` (ZLinkKotlinRequestCall\<TReply\>)

Typed request를 보내고 typed reply를 기다린다. Reified extension이 `KClass<TReply>` 인자를
생략하게 해 준다.

```kotlin
val reply = routeClient
    .requestToChannel<Player>("game.api", GetPlayer("player-1"))
    .timeout(Duration.ofSeconds(3))
    .await()
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | request에만 붙는다 |
| `.timeout(Duration)` | Java 레퍼런스와 동일한 기본값 | reply를 기다리는 상한 |
| `.await()` | terminal(택 1), `suspend fun` | reply 수신까지 기다린다 |
| `.yield()` | terminal(택 1), `suspend fun` | `SPOT_WIDE` User Spot·Instance Spot handler 안에서만 유효하다. Java `yield(...)`의 coroutine bridge일 뿐 임의 suspension을 Yield로 바꾸지 않는다. 그 밖의 실행 context에서는 coroutine을 suspend하거나 operation을 제출하기 전에 `InvalidOperation`으로 완료한다 |

`requestToChannel<TReply>(channelName, request)`/`requestToNode<TReply>(...)`는 reified inline
extension으로, 내부에서 `TReply::class`를 `KClass<TReply>`를 받는 base 오버로드에 전달한다.

**완료 결과.** Java 레퍼런스의 `requestToChannel`/`requestToNode` 완료 kind와 같다.

**선택 기준.** Reply 값이 필요할 때 쓴다. One-way면 `sendToChannel`/`sendToNode`를 쓴다.

---

## `publish` (ZLinkKotlinFanoutClient, classic fanout)

독립 fanout channel에 typed event를 발행한다.

```kotlin
fanoutClient.publish("lobby.events", PlayerJoined("player-1")).await()

// topic을 명시해야 하는 경우
fanoutClient.publish("lobby.events", "region.eu", PlayerJoined("player-1")).await()
```

**옵션.** 반환 타입 `ZLinkKotlinSubmissionCall`에는 `.await()` terminal만 있다.

**완료 결과.** Java 레퍼런스의 classic fanout `publish` 완료 규칙과 같다. 예약된 topic byte(`01 5A
4C 46 31`)를 명시하면 Java runtime의 `ZLinkConfigurationException`을 그대로 던진다.

**선택 기준.** Java 레퍼런스의 `publish`(classic fanout) 항목과 같다.

---

## 공통 실패·취소 규칙 (모든 항목에 적용)

- Queue가 가득 차면 send timeout까지 기다린다. Timeout은 `DeadlineExceeded`, route 단절은
  `Unavailable`, runtime 종료는 `ShuttingDown`으로 완료한다. Target이나 session binding이 없으면
  `NotFound`다.
- Coroutine cancellation이 admission 전에 먼저 확정되면 coroutine cancellation으로 완료하며
  admission을 시작하지 않는다.
- One-way wrapper는 FIFO queue admission을 유지하고 handler를 inline 또는 reentrant하게 호출하지
  않는다.
- `SPOT_WIDE` User Spot·Instance Spot application handler가 아닌 문맥에서 `yield()`를 호출하면
  coroutine을 suspend하거나 underlying operation을 제출하기 전에 `InvalidOperation`으로 완료한다.
  Node direct request, Entry·`PER_ACTOR`, Channel handler와 owner context 밖에도 같은 규칙을
  적용한다.

전체 근거는
[Kotlin channel messaging exact interface](../../common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md)와
[Java 레퍼런스 03. Messaging execution](../../java/reference/03-messaging-execution.ko.md)을
참고한다.
