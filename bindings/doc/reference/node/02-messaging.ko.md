한국어 | [English](02-messaging.en.md)

[레퍼런스 목차](README.ko.md)

# 02. Messaging

이 category는 message 소유권, receive envelope 타입(`Received`,
`TopicMessage`, `SubscriptionEvent`), 그리고 모든 socket type 진입점이
반환하는 공유 send/request/reply operation-builder family를 다룬다. 정확한
signature는
[`contracts/messaging/`](../../../../bindings/node/src/zlink/contracts/messaging/)가
소유한다.

---

## `Message`

message payload 하나를 소유한다. `Message.from(...)`/`Message.allocate(...)`
로 생성된 `Message`는 불변 값 복사본이다 — freeze돼 있으며 명시적 해제가
필요 없다. runtime이 수신한 message만 `close()`가 실제로 해제하는 native
storage를 소유한다.

```ts
const sized = Message.allocate(4096);
const copy = Message.from('payload');
const copyOfBuffer = Message.from(rawBuffer);
```

**Options.**

| Member | 의미 |
| --- | --- |
| `Message.from(buffer: BufferLike \| Message)` | `string`은 UTF-8로 인코딩되고, `Buffer`/`Uint8Array`는 복사되고, 다른 `Message`는 깊은 복사된다 |
| `Message.allocate(size: number)` | 쓰기 가능 storage; 음수나 unsafe-integer 크기면 `RangeError` |
| `data()` | 이 메시지 storage에 backing된 `Buffer`를 반환 |
| `toBytes()` | payload의 독립된 복사 |
| `copy()` | `Message.from(this)`와 동등 |
| `size()` | payload 바이트 길이 |
| `isEmpty()` | `size()`가 0인지 |
| `copyTo(destination, sourceOffset?, destinationOffset?, length?)` | payload(또는 범위)를 caller가 제공한 buffer로 복사, 쓴 byte 수 반환; 범위를 벗어나면 `RangeError` |
| `tryCopyTo(destination)` | destination이 너무 작아도 예외 없이 `boolean`을 반환하는 bounds-check 버전 |
| `getString(encoding = 'utf8')` / `toString()` | payload를 텍스트로 디코딩; `toString()`은 `getString()`과 동등 |
| `refCount()` | native reference count, 진단 전용 |
| `getProperty(name)` | `string \| null` 반환 — **native message metadata는 예약돼 있지만 아직 채워지지 않는다**, 그래서 오늘 시점엔 항상 `null`을 반환한다 |
| `close()` | message를 해제 |

**Completion result.** 모든 member는 동기다. freeze된(factory로 생성된)
메시지에서 `close()`는 no-op다. runtime이 수신한 메시지에선 native
storage를 해제하고 instance를 빈 상태로 리셋한다.

**선택 기준.** caller가 raw 소유권을 유지할 필요가 없는 데이터로 outbound
payload를 만들 땐 `Message.allocate(size)`나 복사하는
`Message.from(...)`를 쓴다. destination 크기가 충분한지 미리 알 수 없을
땐 `copyTo`보다 `tryCopyTo`를 쓴다. `getProperty(...)`는 현재 동작하지
않는 것으로 취급한다 — 예약된 표면이지 실제로 동작하는 metadata 조회가
아니다.

---

## `MessagePartsEnvelope` 공유 기반

`Received`와 `TopicMessage` 둘 다 확장하는 export된 abstract 기반 —
java/cpp의 non-public 대응물과 달리 Node 고유의 public base class다.

**Options.**

| Member | 의미 |
| --- | --- |
| `parts` | `Message[]`, envelope이 소유 |
| `isSinglePart()` | `parts`가 정확히 하나인지 |
| `firstPart()` | 소유권 이전 없이 첫 part; envelope에 part가 없으면 예외 |
| `singlePartOrThrow()` | 단일 part; 정확히 하나가 아니면 예외 |
| `close()` | 모든 part를 닫음 |

**Completion result.** 모든 member는 동기다.

**선택 기준.** 직접 생성하지 않는다 — 아래 `Received`/`TopicMessage`를
쓴다, 둘 다 이 형태를 상속한다.

---

## `Received`

수신된 message envelope: routing 메타데이터, message part, 선택적
reply/send context. 닫힐 때까지 part를 소유한다. receive마다 새로 생성하지
않고 `recv` 호출 전체에서 instance 하나를 재사용한다.

```ts
const received = new Received();
if (dealer.recv(received)) {
  if (received.requestSeq !== null) {
    received.reply().message(Message.from('ok')).submit();
  }
}
```

**Options.** 생성자는 인자를 받지 않는다(무엇이든 넘기면 `TypeError`).
`MessagePartsEnvelope`를 확장.

| Member | 의미 |
| --- | --- |
| `routingId` | `RoutingId \| null`, receive 경로가 제공할 때만 존재 |
| `requestSeq` | `bigint \| null`, reply 가능할 때만 존재 |
| `reply()` | 공유 `ReplyOperation` builder를 시작; envelope에 request sequence/reply context가 없으면 `SubmitError` |
| `send()` | 공유 `SendOperation` builder를 시작, 이 envelope이 포착한 source route로 향함; envelope에 send context가 없으면 `SubmitError` |

**Completion result.** 모든 member는 동기다.

**선택 기준.** message마다 새로 생성하는 대신 receive loop 전체에서
`Received` 하나를 재사용한다. `reply()`를 호출하기 전에 `requestSeq !==
null`로 envelope이 reply 가능한지 확인한다.

---

## `TopicMessage`

수신된 publish: topic과 message part. 닫힐 때까지 part를 소유한다.

```ts
const published = new TopicMessage();
if (sub.subscribe(published)) {
  const topic = published.topic;
}
```

**Options.** 인자 없는 생성자. `MessagePartsEnvelope`를 확장.

| Member | 의미 |
| --- | --- |
| `routingId` | `RoutingId \| null`, publisher의 routing id, receive 경로가 제공할 때만 존재 |
| `topic` | `string`, 이 publish가 전송된 topic — getter가 아니라 순수 mutable 필드 |

**Completion result.** 동기다.

**선택 기준.** `Received`와 같은 방식으로 subscribe-receive loop 전체에서
instance 하나를 재사용한다.

---

## `SubscriptionEvent` / `SubscriptionEntry`

XPUB socket이 관찰한 구독자 한 명의 subscribe·unsubscribe를 보고하고, 활성
구독 항목 하나를 기술한다.

```ts
const evt = new SubscriptionEvent();
if (xpub.receiveSubscriptionEvent(evt)) { /* ... */ }
```

**Options.** `SubscriptionEvent`는 인자 없는 생성자와 mutable 필드를 가진
순수 class다; `SubscriptionEntry`는 class가 아니라 `readonly` 필드를 가진
순수 interface다.

| 타입 | Member | 의미 |
| --- | --- | --- |
| `SubscriptionEvent` | `routingId`(`RoutingId \| null`) | 구독자의 routing id, receive 경로가 제공할 때만 존재 |
| | `topic`(`string`) | subscribe/unsubscribe된 topic |
| | `subscribed`(`boolean`) | subscribe면 `true`, unsubscribe면 `false` |
| `SubscriptionEntry` | `readonly filter: string` | 구독 filter 텍스트 |
| | `readonly isPattern: boolean` | `filter`가 리터럴 접두사가 아니라 패턴 매치인지 |

**Completion result.** 둘 다 async 동작이 없는 순수 데이터 홀더다.
`SubscriptionEvent`는 `close()`가 없다 — native resource를 소유하지
않는다.

**선택 기준.** XPUB socket의 subscription-event receive 경로(Sockets
category)에서 구독자 변동을 관찰할 때 쓴다. `SubscriptionEntry`는
socket의 subscription-snapshot 조회(Sockets category)의 반환 타입이다.

---

## Send / request / reply operation-builder 형태

모든 socket type의 `send`/`publish`/`request`/`reply` 진입점(Sockets
category)이 part·flag·terminal submit을 누적하기 위해 반환하는 fluent
builder. 모든 builder 단계는 세 개의 작은 제네릭 stage interface 중
하나를 확장한다: `PartBuilder<TNext>`(`message(m): TNext`),
`Flaggable<TNext>`(`flags(f): TNext`), `Timeoutable<TNext>`
(`timeout(ms): TNext`).

```ts
dealer.send().message(Message.from('p1')).message(Message.from('p2')).submit();

const reply = await dealer.request()
  .message(Message.from('payload'))
  .timeout(5000)
  .submit();

received.reply().message(Message.from('ok')).submit();
```

**Options.**

| Stage | Member | 의미 |
| --- | --- | --- |
| `SendOperation` | `extends PartBuilder<SendSubmitOperation>` | `.message(m)`이 chain을 시작 |
| `SendSubmitOperation` | `.message(...)` / `.flags(f)` / `submit(): boolean` | part 추가, flag 설정, terminal |
| `RequestOperation` | `extends PartBuilder<RequestSubmitOperation>` | `.message(m)`이 chain을 시작 |
| `RequestSubmitOperation` | `.message(...)` / `.timeout(timeoutMs: number)` | send chain을 미러링하며 reply-wait timeout(Duration류가 아닌 순수 밀리초)을 더함 |
| `RequestSubmitOperation.flags(f)` | `RequestCallbackSubmitOperation`으로 좁혀짐 | `Promise` 반환 `submit()`을 없앰 — `submit(callback: RequestCallback): boolean`만 도달 가능해짐 |
| `RequestSubmitOperation.submit()` | 인자 없는 overload | `Promise<Message[]>` 반환 |
| `ReplyOperation` | `extends PartBuilder<ReplySubmitOperation>` | `.message(m)`이 chain을 시작 |
| `ReplySubmitOperation` | `.message(...)` / `.flags(f)` / `submit(): void` | `Send`를 미러링, timeout 단계 없음 |

**Completion result.** `SendSubmitOperation.submit()`은 동기로
`boolean`을 반환한다(`SendFlags.DontWait`가 설정되고 send가 block됐을
때만 `false` — 그 외 실패는 typed error를 던진다, Errors category).
`ReplySubmitOperation.submit()`은 `void`를 반환한다.
`RequestSubmitOperation.submit()`(인자 없는 형태)은
`Promise<Message[]>`를 반환한다 — caller가 reply message를 소유하며
반드시 close해야 한다. `RequestSubmitOperation.submit(callback)`/
`RequestCallbackSubmitOperation.submit(callback)`은 `boolean`을
반환하고(같은 `DontWait` 관례) `(result, parts)`를 나중에
`RequestCallback`에 전달한다 — `result`가 성공값일 때만 콜백이 `parts`를
소유한다. 모든 builder는 성공적인 submit에서만 누적된 `Message` part를
소비한다 — 실패 시 소유권은 caller에게 복원된다.

**선택 기준.** 일반 `async`/`await` 코드에선 인자 없는 `submit()`의
`Promise`를 쓴다. promise 대신 callback-completion 표면이 필요할 땐
`.flags(...).submit(callback)`을 쓴다. 목적지 route를 손으로 재구성하는
대신 `Received.reply()`/`send()`를 쓴다.

---

## Handler type alias

Sockets/Eventing category 전반에서 콜백 인자로 쓰이는 함수 타입 alias.

| Alias | 등록하는 곳 | Signature |
|---|---|---|
| `SocketSendReadyHandler` | socket의 send-ready 등록(Sockets category) | `() => void` |
| `StreamPacketHandler` | `StreamSocket`의 packet 등록(Sockets category) | `(sourceRid: RoutingId, header: Message, body: Message) => void` — 두 메시지 모두 소유 |
| `SocketMonitorHandler` | `SocketMonitor.onEvent(...)`(Eventing category) | `(event: MonitorEvent) => void` |
| `RequestCallback` | `RequestSubmitOperation.submit(callback)`/`RequestCallbackSubmitOperation.submit(callback)`(위) | `(result: RequestResult, parts: readonly Message[]) => void` — 성공일 때만 `parts` 소유 |
| `ReplyHandler` | 이 레퍼런스 트리에 문서화된 어떤 public 진입점으로도 현재 도달하지 않음 | `(result: RequestResult, parts: Message[]) => void` |

---

[`contracts/messaging/`](../../../../bindings/node/src/zlink/contracts/messaging/)와
[Node 바인딩 스펙](../../spec/node/README.ko.md)에서 전체 근거를 확인한다.
