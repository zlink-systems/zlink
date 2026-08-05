한국어 | [English](02-messaging.en.md)

[레퍼런스 목차](README.ko.md)

# 02. Messaging

이 category는 message 소유권과 receive envelope 타입(`Received`,
`ReceivedMessage`, `TopicMessage`, `SubscriptionEvent`)을 다룬다.
**send/request/reply operation-builder family는 여기 선언돼 있지 않다**
— 지금까지 다룬 다른 모든 언어와 달리, 그 Protocol(`SendOp`,
`RequestOp`, `RequestCallbackOp`, `ReplyOp`)은
`contracts/sockets/operations.py`에 있으며 대신 Sockets category에
문서화된다. 정확한 signature는
[`contracts/messaging/`](../../../../bindings/python/src/zlink/contracts/messaging/)가
소유한다.

---

## `Message`

message payload. sync·async context-manager 프로토콜 둘 다 지원한다 —
닫으면(또는 `with`/`async with` block을 떠나면) payload가 해제된다.
메시지를 보내면 소비된다.

```python
sized = Message.allocate(4096)
copy = Message.from_(b"payload")
with copy:
    view = copy.data
```

**Options.**

| Member | 의미 |
| --- | --- |
| `from_(data)` | class method; 임의 bytes-like `data`의 독립된 복사; `from`이 키워드라 뒤에 underscore가 붙음 |
| `allocate(size: int)` | class method; 쓰기 가능 storage |
| `copy()` | 독립된 payload 복사 |
| `size()` | payload 바이트 길이 |
| `is_empty()` | `size()`가 0인지 |
| `data` | property, zero-copy `memoryview`, 메시지가 열려있는 동안만 유효 |
| `to_bytes()` | payload의 `bytes` 복사 |
| `copy_to(destination, source_offset=0, destination_offset=0, length=None)` | payload(또는 범위)를 caller가 제공한 buffer로 복사, 쓴 byte 수 반환 |
| `try_copy_to(destination)` | 쓴 byte 수를 반환, `destination`이 너무 작으면 `None` — `copy_to`의 예외를 던지지 않는 대안 |
| `to_string(encoding="utf-8")` | payload를 텍스트로 디코딩 |
| `ref_count()` | native reference count, 진단 전용 |
| `close()` | message를 해제 |

**Completion result.** 모든 member는 동기다. `Message`는
`with`/`async with` 둘 다 지원한다.

**선택 기준.** outbound payload를 만들 땐 `Message.allocate(size)`나
복사하는 `Message.from_(data)`를 쓴다. 독립된 복사가 필요 없을 땐
`to_bytes()`보다 zero-copy `data` `memoryview`를 선호한다 — 다만
메시지가 열려있는 동안만이다, view가 메시지 자신의 storage를 그대로
가리키기 때문이다.

---

## `ReceivedMessage`

`Message`와 구별되는 수신된 message part 하나 —
`ReceivedMultipart`/`Received` envelope을 iterate할 때 나오는 타입.

**Options.** 인자 없음 — envelope을 iterate해서만 얻으며 직접 생성하지
않는다.

| Member | 의미 |
| --- | --- |
| `__len__` | `len(part)`을 통한 part 크기(byte) |
| `data` | property, `memoryview` snapshot |
| `to_bytes()` | part의 `bytes` 복사 |
| `close()` | part를 해제 |

**Completion result.** 동기다. sync·async context-manager 프로토콜
둘 다 지원한다.

**선택 기준.** 별도 parts 목록을 인덱싱하는 대신 `Received`/
`TopicMessage` envelope을 iterate해서(`for part in received:`) 각
`ReceivedMessage`를 순서대로 접근한다.

---

## `ReceivedMultipart` / `Received`

`ReceivedMultipart`는 공유 multipart-envelope 형태다(part를 iterate
하거나 인덱싱). `Received`는 여기에 routing 메타데이터와 선택적
reply/send context를 더해 확장한다. 둘 다 닫힐 때까지 part를
소유한다.

```python
received = create_received()
if dealer.recv_into(received):
    if received.is_single_part():
        pass
    received.reply().message(b"ok").submit()
```

**Options.** **`Received`도 `ReceivedMultipart`도 이 contract에서
`routing_id`/`request_seq`를 문서화된 public member로 노출하지
않는다** — reply/send context는 `reply()`/`send()` 메서드 자체를
통해서만 도달한다.

| 타입 | Member | 의미 |
| --- | --- | --- |
| `ReceivedMultipart` | `__iter__` | part를 순서대로 iterate |
| | `__len__` | `len(envelope)`을 통한 part 개수 |
| | `to_bytes_list()` | part별 `bytes` 복사 목록 |
| | `is_single_part()` | envelope이 part를 정확히 하나 가졌는지 |
| | `first_part()` | 소유권 이전 없이 첫 part; 비어있으면 `RecvError` |
| | `single_part_or_throw()` | 단일 part; 정확히 하나가 아니면 `RecvError` |
| | `close()` | 소유한 모든 part를 닫음 |
| `Received extends ReceivedMultipart` | `send()` | 공유 `SendOp` builder(Sockets category)를 시작, 이 envelope이 포착한 source route로 향함; envelope에 send context가 없으면 `SubmitError` |
| | `reply()` | 공유 `ReplyOp` builder를 시작; envelope이 reply 가능하지 않으면 `SubmitError` |

**Completion result.** 모든 member는 동기다. 둘 다 sync·async
context-manager 프로토콜을 지원한다.

**선택 기준.** message마다 새로 생성하는 대신 receive loop 전체에서
(`create_received()`로) `Received` 하나를 재사용한다. envelope이
reply 가능한지·send context가 있는지 미리 테스트할 별도의
boolean/property가 없으므로, `reply()`/`send()`가 예외를 던지는지로
판단한다.

---

## `TopicMessage`

수신된 publish: topic과 message part. `ReceivedMultipart` 자체가
아니라(`ReceivedMultipart`도 거기서 만들어지는) 같은 공유 형태인
`_BaseReceived`를 확장한다. 닫힐 때까지 part를 소유한다.

```python
published = create_topic_message()
if sub.subscribe_into(published):
    topic = published.topic
```

**Options.**

| Member | 의미 |
| --- | --- |
| `topic` | property, get **그리고 set** — 다른 모든 언어의 읽기 전용 `topic`과 다름; 이 publish가 전송된 topic |
| `__iter__` / `__len__` / `to_bytes_list()` / `is_single_part()` / `first_part()` / `single_part_or_throw()` / `close()` | `ReceivedMultipart`와 같은 형태 |

**Completion result.** 동기다. sync·async context-manager 프로토콜을
지원한다.

**선택 기준.** `Received`와 같은 방식으로 subscribe-receive loop
전체에서(`create_topic_message()`로) instance 하나를 재사용한다.

---

## `SubscriptionEvent`

XPUB socket이 관찰한 구독자 한 명의 subscribe·unsubscribe를 보고한다.

```python
evt = create_subscription_event()
if xpub.receive_subscription_event_into(evt):
    ...
```

**Options.** 이 `Protocol`은 docstring 설명("its routing id, topic,
and whether it subscribed") 외엔 자신의 member를 선언하지 않는다 —
실제 runtime 구현은 `routing_id`/`topic`/`subscribed` 형태의 속성을
노출하지만, 이 contract 파일 자체는 위 `Message`/`Received`/
`TopicMessage`처럼 표면을 명시적으로 나열하지 않고 아무것도 typed
member로 선언하지 않는다.

**Completion result.** 해당 없음 — 여기서 계약상 명시된 member가
없다.

**선택 기준.** XPUB socket의 subscription-event receive 경로(Sockets
category)에서 구독자 변동을 관찰할 때 쓴다. 이 Protocol이 속성 이름을
나열하지 않으므로 정확한 속성 이름은 runtime 구현이나 스펙을 직접
참고한다.

---

[`contracts/messaging/`](../../../../bindings/python/src/zlink/contracts/messaging/)와
[Python 바인딩 스펙](../../spec/python/README.ko.md)에서 전체 근거를 확인한다.
