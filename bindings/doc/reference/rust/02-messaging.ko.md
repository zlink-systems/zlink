한국어 | [English](02-messaging.en.md)

[레퍼런스 목차](README.ko.md)

# 02. Messaging

이 category는 message 소유권, receive envelope 타입(`Received`,
`TopicMessage`, `SubscriptionEvent`), 그리고 모든 socket type 진입점이
반환하는 공유 send/request/reply operation-builder family를 다룬다. 정확한
signature는
[`contracts/messaging/`](../../../../bindings/rust/src/contracts/messaging/)가
소유한다.

---

## `Message`

native zlink frame 하나를 소유한다 — 모든 send·request·reply·receive
API가 옮기는 단위다.

```rust
let empty = Message::new()?;
let sized = Message::with_size(4096)?;
let copy: Message = "payload".try_into()?;
```

**Options.** 생성자와 변환은 전부 `Result<Self, ConfigError>`를 반환하며,
해당하는 경우 입력을 복사한다.

| Member | 의미 |
| --- | --- |
| `Message::new()` | 빈 메시지 |
| `Message::with_size(size: usize)` / `Message::allocate(size)` | 쓰기 가능, 초기화 안 된 storage; `allocate`는 `with_size`의 alias |
| `Message::try_from<T: AsRef<[u8]>>(data)` / `TryFrom<&[u8]>` / `TryFrom<Vec<u8>>` / `TryFrom<&str>` | 입력을 message가 소유하는 storage로 복사 |
| `as_bytes()` / `data_mut()` | 이 instance의 storage에 backing된 읽기 전용/쓰기 가능 byte-slice view |
| `size()` | payload 바이트 길이 |
| `is_empty()` | `size()`가 0인지 |
| `as_str() -> Result<&str, std::str::Utf8Error>` | payload를 UTF-8로 디코딩 |
| `to_vec()` | payload의 `Vec<u8>` 복사 |
| `copy_to(&mut [u8]) -> Result<usize, ConfigError>` | payload를 caller가 제공한 slice로 복사; destination이 너무 작으면 error — 다른 언어와 달리 error 없는 `try_copy_to` 변형이 여기엔 없다 |
| `ref_count()` | native reference count, 진단 전용 |
| `try_clone() -> Result<Self, ConfigError>` | 독립된 payload 복사 |

**Completion result.** 모든 member는 동기다. `Message`는 `Drop`을
구현해서 native storage를 자동으로 해제한다. 메시지를 보내면 native
frame이 socket으로 이전되고(아래 operation-builder 형태의 chain에서
소비됨), 그 후엔 payload를 읽어도 더 이상 의미가 없다.

**선택 기준.** outbound payload를 만들 땐 `Message::with_size`/`allocate`나
`TryFrom`/`try_from` 변환을 쓴다. 소유권을 옮기는 대신 독립된 복사가
필요할 땐 `try_clone()`을 쓴다. `copy_to`가(`bool`을 반환하는
`try_copy_to`가 아니라) `Result`를 반환한다는 건 destination이 작을 수
있을 때 caller가 `ConfigError` 케이스를 명시적으로 처리해야 한다는
뜻이다.

---

## `Received`

수신된 message envelope: routing 메타데이터와 message part. drop되거나
`close`될 때까지 part를 소유한다. receive마다 새로 생성하지 않고 `recv`
호출 전체에서 instance 하나를 재사용한다.

```rust
let mut received = Received::empty();
if dealer.recv(&mut received, RecvFlags::NONE)? {
    if received.request_seq().is_some() {
        received.reply().message(Message::try_from("ok")?).submit()?;
    }
}
```

**Options.** `Received::empty()`가 caller-provided 재사용 storage를 위한
유일한 public 생성자다.

| Member | 의미 |
| --- | --- |
| `is_single_part()` | `parts()`가 정확히 하나인지 |
| `routing_id()` | `Option<&RoutingId>`, receive 경로가 제공할 때만 존재 |
| `request_seq()` | `Option<u64>`, reply 가능할 때만 존재 |
| `parts()` | `&[Message]`, 이 envelope이 담은 모든 message part |
| `first_part() -> Result<&Message, RecvError>` | 소유권 이전 없이 첫 part |
| `single_part()` / `single_part_or_error()` | 동등 — 둘 다 `self`를 소비하고 `Result<Message, RecvError>`를 반환, 정확히 하나가 아니면 error |
| `into_parts() -> Vec<Message>` | `self`를 소비하며 모든 part의 소유권을 이전 |
| `close(self) -> Result<(), CloseError>` | `self`를 소비, 소유한 모든 part를 닫음 |
| `reply()` | 공유 `ReplyOp<Empty>` builder를 시작; request sequence가 있는 envelope에서만 유효 |
| `send()` | 공유 `SendOp<Empty>` builder를 시작, 이 envelope이 포착한 source route로 향함 |

**Completion result.** 모든 member는 동기다. 여러 메서드(`single_part`,
`into_parts`, `close`)가 `self`를 값으로 받아 envelope을 소비한다 —
Rust의 소유권 시스템이 컴파일 타임에 소비된 `Received`가 재사용될 수
없음을 강제한다, 소비 후 재사용이 런타임 계약일 뿐인 다른 언어와 다르다.

**선택 기준.** message마다 새로 생성하는 대신 receive loop 전체에서(`&mut`
로) `Received` 하나를 재사용한다. 소비하지 않는 조회엔 `first_part()`를,
envelope 자체가 더 이상 필요 없고 part를 꺼내야 할 땐
`single_part()`/`into_parts()`를 쓴다.

---

## `TopicMessage`

수신된 publish: topic, source routing id, message part. drop되거나
`close`될 때까지 part를 소유한다.

```rust
let mut published = TopicMessage::empty();
if sub.subscribe(&mut published, RecvFlags::NONE)? {
    let topic = published.topic();
}
```

**Options.** `TopicMessage::empty()`가 유일한 public 생성자다. Instance
member는 `Received`의 형태를 그대로 반영한다.

| Member | 의미 |
| --- | --- |
| `is_single_part()` | `parts()`가 정확히 하나인지 |
| `topic() -> &str` | 이 publish가 전송된 topic |
| `routing_id()` | `Option<&RoutingId>`, publisher의 routing id, receive 경로가 제공할 때만 존재 |
| `parts()` | `&[Message]`, 이 publish가 담은 모든 message part |
| `first_part()` / `single_part()` / `single_part_or_error()` / `into_parts()` / `close(self)` | `Received`와 같은 형태 |

**Completion result.** 동기다. `Received`와 같은 소비/비소비 member
구분이다.

**선택 기준.** `Received`와 같은 방식으로 subscribe-receive loop 전체에서
instance 하나를 재사용한다.

---

## `SubscriptionEvent`

XPUB socket이 관찰한 구독자 한 명의 subscribe·unsubscribe를 보고한다.

```rust
let mut evt = SubscriptionEvent::empty();
if xpub.receive_subscription_event(&mut evt, RecvFlags::NONE)? { /* ... */ }
```

**Options.** `SubscriptionEvent::empty()`가 유일한 public 생성자다.

| Member | 의미 |
| --- | --- |
| `routing_id()` | `Option<&RoutingId>`, 구독자의 routing id, receive 경로가 제공할 때만 존재 |
| `topic() -> &str` | subscribe/unsubscribe된 topic |
| `is_subscribed() -> bool` | subscribe면 `true`, unsubscribe면 `false` |

**Completion result.** 동기다. `close()`가 없다 — 이 타입은 자신의
native resource를 소유하지 않는다.

**선택 기준.** XPUB socket의 subscription-event receive 경로(Sockets
category)에서 구독자 변동을 관찰할 때 쓴다.

---

## `SendResult`

non-blocking send의 결과, 작은 standalone enum으로.

**Options.**

| Member | 의미 |
| --- | --- |
| `Sent` | send가 즉시 완료됨 |
| `Backpressured` | send가 block됐을 것 |
| `NotReady` | destination이 아직 send를 받을 준비가 안 됨 |
| `is_sent() -> bool` | `matches!(self, SendResult::Sent)`의 편의 축약형 |

**Completion result.** 해당 없음 — 이 레퍼런스 tier에 문서화된 어떤
진입점도 이걸 직접 반환하지 않는 순수 값 타입이다. 이 category
소스에서 `SendOp::submit()`(아래)이 실제로 반환하는 `bool`과 구별되는
public 타입으로 존재한다.

**선택 기준.** 여기 문서화된 builder 기반 send 경로가 직접 만들어내는
게 아니다 — 더 저수준 진입점이 이 타입을 반환하는지는 Sockets
category를 참고한다.

---

## Send / request / reply operation-builder 형태

모든 socket type의 `send`/`publish`/`request`/`reply` 진입점(Sockets
category)이 part·flag·terminal submit을 누적하기 위해 반환하는
**typestate 기반** fluent builder. 지금까지 다룬 다른 모든 언어(builder
단계마다 별개의 interface/class 타입을 쓰는, 예:
`SendOperation`/`SendSubmitOperation`)와 달리, Rust는 단계 전환을
컴파일러가 정적으로 추적하는 zero-sized marker 타입(`Empty`, `Ready`,
`CallbackReady`)으로 매개변수화된 단일 제네릭 타입(`SendOp<State>`,
`RequestOp<State>`, `ReplyOp<State>`)으로 표현한다. 각
`impl SendOp<Empty> { ... }`/`impl SendOp<Ready> { ... }` block은 그
단계에서 유효한 메서드만 노출한다.

```rust
dealer.send().message(part1)?.message(part2)?.submit()?;

dealer.request()
    .message(Message::try_from("payload")?)
    .timeout(Duration::from_secs(5))
    .submit(|result| {
        // 나중에 전달됨; result: Result<Vec<Message>, RequestError>
    })?;

received.reply().message(Message::try_from("ok")?).submit()?;
```

**Options.**

| Stage | Member | 의미 |
| --- | --- | --- |
| `SendOp<Empty>` | `.message(self, Message) -> SendOp<Ready>` | chain을 시작, `self`를 소비하고 다음 단계 타입을 반환 |
| `SendOp<Ready>` | `.message(...)` / `.flags(self, SendFlags) -> Self` / `.submit(self) -> Result<bool, SubmitError>` | part 추가, flag 설정, terminal |
| `RequestOp<Empty>` → `RequestOp<Ready>` | `SendOp`와 같음 + `.timeout(self, Duration) -> Self` | send chain을 미러링하며 reply-wait timeout을 더함 |
| `RequestOp<Ready>.flags(self, SendFlags)` | `RequestOp<CallbackReady>`로 좁혀짐 | 같은 `message`/`timeout`/`flags`/`submit` 메서드를 다시 노출 — 그 단계에서 flag를 여러 번 설정할 수 있다 |
| `RequestOp<Ready>::submit` / `RequestOp<CallbackReady>::submit` | `submit<F>(self, callback: F) -> Result<(), SubmitError> where F: FnOnce(Result<Vec<Message>, RequestError>) + Send + 'static` | **둘 다 callback 전용** — 지금까지 다룬 다른 모든 언어와 달리 이 binding의 public contract엔 `Future`/async를 반환하는 overload가 없다 |
| `ReplyOp<Empty>` → `ReplyOp<Ready>` | `SendOp`를 반영, `.submit(self) -> Result<(), SubmitError>` | `.flags(self, SendFlags) -> Self` 외엔 자신만의 flags-narrowing 동작이 없다 |

**Completion result.** `SendOp::submit()`은 `Result<bool, SubmitError>`
를 반환한다 — 내부 `bool`은 `SendFlags::DONT_WAIT`가 설정되고 send가
block됐을 때만 `false`다(back-pressure) — 다른 모든 실패는 `Err`
variant다. `ReplyOp::submit()`은 `Result<(), SubmitError>`를 반환한다.
`RequestOp::submit(callback)`은 *dispatch* 자체(request가 성공적으로
제출됐는지)에 대해 `Result<(), SubmitError>`를 반환한다 — 실제
reply나 실패는 나중에 background dispatch 스레드에서 `callback`에
`Result<Vec<Message>, RequestError>`로 전달된다. 모든 builder는 성공적인
submit에서만 누적된 `Message` part를 소비한다.

**선택 기준.** Rust엔 여기 async submit 경로가 없으므로, 모든
request/reply엔 callback 형태를 쓴다 — 호출부에서 `async`/`.await`
어법이 필요하면 async 런타임의 channel/oneshot으로 직접 연결한다.
목적지 route를 손으로 재구성하는 대신 `Received.reply()`/`send()`를
쓴다.

---

[`contracts/messaging/`](../../../../bindings/rust/src/contracts/messaging/)와
[Rust 바인딩 스펙](../../spec/rust/README.ko.md)에서 전체 근거를 확인한다.
