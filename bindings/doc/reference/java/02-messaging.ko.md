한국어 | [English](02-messaging.en.md)

[레퍼런스 목차](README.ko.md)

# 02. Messaging

이 category는 message 소유권, receive envelope 타입(`Received`, `TopicMessage`,
`SubscriptionEvent`), 그리고 모든 socket type 진입점이 반환하는 공유
send/request/reply operation-builder family를 다룬다. 정확한 signature는
[`contracts/messaging/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/messaging/)가
소유한다.

---

## `Message`

native zlink frame 하나를 소유한다 — 모든 send·request·reply·receive API가 옮기는
단위다. Java는 borrowed-payload wrapper를 노출하지 않는다 — native queue lifetime이
Java object reachability로 안전하게 bound되지 않기 때문이다 — 모든 `from(...)`
factory는 message 소유 storage로 복사한다.

```java
Message empty = new Message();
Message sized = new Message(4096);
Message copy = Message.from("payload".getBytes(StandardCharsets.UTF_8));
Message fromString = Message.from("hello");
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `Message()` | 빈 메시지 |
| `Message(int size)` | 쓰기 가능 storage |
| `allocate(int)` | static factory, `Message(int)`와 동일 |
| `from(byte[])` / `from(byte[] data, int offset, int length)` | 전체 배열 복사, 또는 선택한 범위 복사 |
| `from(Message)` | 다른 메시지의 payload 복사 |
| `from(String)` | UTF-8 인코딩 |
| `from(ByteBuffer)` | source cursor를 바꾸지 않고 남은 byte 복사 |
| `from(io.netty.buffer.ByteBuf)` | Netty interop, source cursor를 바꾸지 않고 readable byte 복사 |
| `size()` | payload의 바이트 길이 |
| `more()` | multipart-continuation flag |
| `refCount()` | native reference count, 진단 전용 |
| `empty()`/`isEmpty()` | `size()`가 0인지 |
| `data()`/`toByteArray()` | payload를 `byte[]`로 복사 |
| `toUtf8String()` | payload를 UTF-8로 디코딩 |
| `dataBuffer()` | 이 인스턴스 storage에 backing된 읽기 전용 `ByteBuffer` view |
| `mutableDataBuffer()` | 이 인스턴스 storage에 backing된 쓰기 가능 `ByteBuffer` view |
| `copyTo(byte[])` / `copyTo(byte[], int offset)` / `copyTo(byte[] dst, int srcOffset, int dstOffset, int length)` | payload(또는 범위)를 caller가 제공한 배열로 복사 |
| `copyTo(ByteBuffer)` / `copyTo(ByteBuf)` | payload를 caller가 제공한 buffer로 복사 |
| `tryCopyTo(ByteBuffer)` / `tryCopyTo(ByteBuf)` | bounds-check 버전, 예외 대신 `boolean` 반환 |
| `copyFrom(byte[]|Message, int srcOffset, int dstOffset, int length)` | source에서 이 메시지 storage로 byte 복사 |
| `readByte`/`readIntLe`/`readIntBe`/`readLongLe`/`writeByte`/`writeIntLe`/`writeIntBe`/`writeLongLe` | wire format을 message storage에 직접 파싱·쓰기하는 in-place binary accessor |
| `fill(byte)` / `fill(byte, offset, length)` | payload(또는 범위)를 반복되는 byte로 덮어씀 |
| `contentEquals(byte[])` | payload 동등성 확인 |
| `closeAll(Message[])` / `closeAll(Iterable<? extends Message>)` | static; 한 호출로 모든 part를 닫으며 개별 close 실패는 조용히 무시 |

**완료 결과.** 모든 member는 동기다. `Message implements AutoCloseable`이다. 메시지를
보내면 native frame이 socket으로 이전돼 이후 읽기에 대해 instance가 무효화된다 —
보내지 않을 메시지를 해제하려면 `close()`를 쓴다. 범위를 벗어난 offset/length는
`IndexOutOfBoundsException`을 던진다.

**선택 기준.** outbound payload를 만들 땐 크기 지정 생성자나 복사하는 `from(...)`
factory를 쓴다. wire format을 중간 `byte[]` 없이 message storage에 직접
파싱·쓰기하려면 in-place binary accessor(`readIntLe` 등)를 쓴다. 수신되거나
구성된 multipart 배열의 모든 part를 손으로 짠 loop 대신 한 호출로 해제하려면
`closeAll(...)`을 쓴다.

---

## `Received`

recv 결과 하나를 집계한다: 선택적 routing id, request sequence, 소유한 message
part. 반환된 `parts()` view는 불변이며 밑에 깔린 배열을 복사하지 않는다.

```java
Received received = new Received();
if (dealer.recv(received)) {
    received.requestSeq().ifPresent(seq ->
        received.reply().message(Message.from("ok")).submit());
}
```

**옵션.** caller-provided storage용 public 인자 없는 생성자 `Received()` —
binding이 매 성공적인 receive마다 내부 상태를 그 자리에서 덮어쓴다(receive마다
할당을 피함).

| Member | 반환 | 의미 |
| --- | --- | --- |
| `getRoutingId()` | `Optional<RoutingId>` | receive 경로가 제공할 때만 존재 |
| `requestSeq()` | `Optional<Long>` | reply 가능할 때만 존재 |
| `parts()` | `List<Message>`, 불변 view | 이 envelope이 담은 모든 message part |
| `isSinglePart()` | `boolean` | `parts()`가 정확히 하나인지 |
| `firstPart()` | `Message` | 첫 part, 소유권 이전 없음 |
| `singlePartOrThrow()` | `Message` | 단일 part, `parts()`가 정확히 하나가 아니면 예외 |
| `reply()` | builder | 공유 `ReplyOperation` 시작; 유효한 reply context가 없으면 `submit()`에서 `ZlinkSubmitException` |
| `send()` | builder | 공유 `SendOperation` 시작, 이 envelope이 포착한 source route로 향함 |

`Received implements AutoCloseable`이며, `close()`는 소유한 모든 part를 닫는다.

**완료 결과.** 모두 동기다. `firstPart()`/`singlePartOrThrow()`는 각각 데이터가
없거나 part 개수가 맞지 않을 때 `ZlinkRecvException`을 던진다 — Errors
category에 문서화된 receive측 result code를 그대로 반영한다.

**선택 기준.** message마다 새로 생성하는 대신 receive loop 전체에서 `Received`
하나를 재사용한다. `reply()`를 호출하기 전에 `requestSeq()`로 envelope이 실제로
reply 가능한지 확인한다.

---

## `TopicMessage`

raw subscription 경로가 쓰는 topic-aware recv 결과 — 수신된 publish의 topic,
source routing id, message part.

```java
TopicMessage published = new TopicMessage();
if (sub.subscribe(published)) {
    String topic = published.topic();
}
```

**옵션.** public 인자 없는 생성자 `TopicMessage()`.

| Member | 반환 | 의미 |
| --- | --- | --- |
| `getRoutingId()` | `Optional<RoutingId>` | 발행자의 routing id, receive 경로가 제공할 때만 존재 |
| `topic()` | `String` | 이 publish가 전송된 topic |
| `parts()` | `List<Message>` | 이 publish가 담은 모든 message part |
| `isSinglePart()` / `firstPart()` / `singlePartOrThrow()` | — | `Received`와 같은 형태 |

`TopicMessage implements AutoCloseable`.

**완료 결과.** 동기다. `firstPart()`/`singlePartOrThrow()`는 `Received`의 대응
메서드와 같은 방식으로 `ZlinkRecvException`을 던진다.

**선택 기준.** `Received`와 같은 방식으로 subscribe-receive loop 전체에서
instance 하나를 재사용한다.

---

## `SubscriptionEvent` / `SubscriptionEntry`

XPUB socket이 관찰한 구독자 한 명의 subscribe·unsubscribe를 보고하고, 활성 구독
항목 하나를 기술한다.

```java
SubscriptionEvent evt = new SubscriptionEvent();
if (xpub.receiveSubscriptionEvent(evt)) { /* ... */ }
```

**옵션.** `SubscriptionEvent()` public 인자 없는 생성자.

| 타입 | Member | 의미 |
| --- | --- | --- |
| `SubscriptionEvent` | `getRoutingId()`(`Optional<RoutingId>`) | 구독자의 routing id, receive 경로가 제공할 때만 존재 |
| | `topic()`(`String`) | 구독·구독 취소된 topic |
| | `subscribed()`(`boolean`) | 구독이면 `true`, 구독 취소면 `false` |
| `SubscriptionEntry(String filter, boolean pattern)` | record | 활성 구독 하나 |
| | `filterBytes()` | `filter`를 UTF-8로 인코딩 |
| | `fromBytes(byte[], boolean)` | `filter`를 UTF-8 byte에서 복원하는 static factory |

**완료 결과.** 둘 다 async 동작이 없는 순수 데이터 홀더다. `SubscriptionEvent`는
`close()`가 없다 — native resource를 소유하지 않는다.

**선택 기준.** XPUB socket의 subscription-event receive 경로(Sockets
category)에서 구독자 변동을 관찰할 때 쓴다. `SubscriptionEntry`는 socket의
subscription-snapshot 조회(Sockets category)의 반환 타입이다.

---

## Send / request / reply operation-builder 형태

모든 socket type의 `send`/`publish`/`request`/`reply` 진입점(Sockets
category)이 part·flag·terminal submit을 누적하기 위해 반환하는 fluent builder.
모든 builder interface는 공유 `MessageBuilderStage<TSubmit>`(`TSubmit
message(Message part)`)를 확장하며, request family는 추가로
`TimeoutSubmitOperation<TResult, TCallback>`을 확장한다.

```java
dealer.send().message(part1).message(part2).submit();

CompletionStage<List<Message>> future = dealer.request()
    .message(Message.from("payload"))
    .timeout(Duration.ofSeconds(5))
    .submit();
List<Message> reply = future.toCompletableFuture().join();

// 또는 virtual thread에서:
List<Message> reply2 = dealer.request().message(Message.from("payload")).await();

received.reply().message(Message.from("ok")).submit();
```

**옵션.**

| 단계 | Member | 의미 |
| --- | --- | --- |
| `SendOperation` | `.message(Message)` | chain 시작 |
| `SendSubmitOperation` | `.message(...)` / `.flags(SendFlags)` / `.submit()` | part 추가, flag 설정, terminal |
| `RequestOperation`/`RequestSubmitOperation` | `Send`와 동일 + `.timeout(Duration)` | send chain을 그대로 반영하며 reply 대기 timeout을 더함 |
| `RequestSubmitOperation.flags(SendFlags)` | `RequestCallbackSubmitOperation`으로 좁힘 | `CompletionStage`를 반환하는 `.submit()`이 사라짐 — 이후 `.submit(RequestCallback)`만 도달 가능 |
| `ReplyOperation`/`ReplySubmitOperation` | `Send`와 같은 형태 | flags 단계가 없음 — 밑바탕 reply 함수가 send-flag 인자를 받지 않음 |
| `TimeoutSubmitOperation.await()` | `default` 메서드 | submit하고 결과가 완료될 때까지 현재 스레드를 block — 명시적으로 virtual thread를 위한 것이다(virtual thread를 parking하면 carrier platform thread가 풀리는 반면, platform thread를 직접 block하는 것과 다르다) — framework 자신의 async 경로는 대신 `submit()`을 쓴다 |

**완료 결과.**

| Terminal | 반환 | 의미 |
| --- | --- | --- |
| `SendSubmitOperation.submit()` | `boolean` | `SendFlags.DONT_WAIT`가 설정되고 send가 block됐을 때만 `false`, 그 외 실패는 `ZlinkException` |
| `ReplySubmitOperation.submit()` | `void` | 실패하면 `ZlinkException`을 던짐 |
| `RequestSubmitOperation.submit()` | `CompletionStage<List<Message>>` | caller가 reply message를 소유하며 반드시 close해야 함 |
| `submit(RequestCallback)`(`RequestSubmitOperation`/`RequestCallbackSubmitOperation`) | `boolean` | 같은 `DONT_WAIT` 관례; 결과와 part를 나중에 콜백에 전달 — 결과가 `RequestResult.OK`일 때만 콜백이 part를 소유 |

모든 builder는 성공적인 submit에서만 누적된 `Message` part를 소비한다 — 실패
시 소유권은 caller에게 복원된다.

**선택 기준.** 일반 async 코드에선 `submit()`의 `CompletionStage`를 쓴다.
순차 호출처럼 자연스럽게 읽히는 코드가 필요한 virtual thread에선 `await()`을
쓴다. 전혀 block·park해선 안 되는 스레드에서 callback-completion 표면이
필요할 땐 `.flags(...).submit(callback)`을 쓴다. 목적지 route를 손으로
재구성하는 대신 `Received.reply()`/`send()`를 쓴다.

---

[`contracts/messaging/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/messaging/)와
[Java 바인딩 스펙](../../spec/java/README.ko.md)에서 전체 근거를 확인한다.
