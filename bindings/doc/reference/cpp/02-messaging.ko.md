한국어 | [English](02-messaging.en.md)

[레퍼런스 목차](README.ko.md)

# 02. Messaging

이 category는 message 소유권, receive envelope 타입(`received_t`, `topic_message_t`,
`subscription_event_t`), 그리고 모든 socket type의
`send`/`publish`/`request`/`reply`가 반환하는 공유 send/request/reply move-only
builder family를 다룬다. 정확한 signature는
[`Contracts/Messaging/`](../../../../bindings/cpp/include/zlink/Contracts/Messaging/)가
소유한다. `lazy_message_parts.hpp`와 `operation_builder_base.hpp`는
`namespace zlink::detail`에 있어 public contract 항목이 없다.

---

## `message_t`

zlink message payload 하나를 소유한다 — 모든 send·request·reply·receive API가 옮기는
단위다.

```cpp
zlink::message_t empty;
zlink::message_t sized (4096);
zlink::message_t copy = zlink::message_t::from (std::string ("payload"));
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `message_t()` | 빈 메시지 |
| `explicit message_t(size_t size_)` | 쓰기 가능 storage |
| `allocate(size_t)` | static factory |
| `from(const std::vector<uint8_t>&)` / `from(std::span<const std::byte>)` / `from(std::span<const uint8_t>)` | static factory, 복사 |
| `from(const std::string&)` | UTF-8 인코딩 |
| `from_json`/`from_messagepack`/`from_protobuf`, `parse_json`/`parse_messagepack`/`parse_protobuf` | template factory·parser; framework codec extension에 위임, 이 패키지 소속 아님 |
| `data()`/`bytes()` | 이 인스턴스 storage에 backing된 쓰기/읽기 전용 view(mutable·`const` overload) |
| `size()` | payload의 바이트 길이 |
| `is_empty()` | `size()`가 0인지 |
| `ref_count()` | native reference count, 진단 전용 |
| `to_bytes()` | payload의 소유 복사본 |
| `copy_to(std::span<std::byte>)` / `copy_to(std::span<uint8_t>)` | payload를 caller가 제공한 buffer로 복사 |
| `to_string()` | payload를 UTF-8로 디코딩 |
| `close()` | payload storage를 해제; 이미 소비된 메시지에도 호출해도 안전 |

복사 생성·대입은 payload를 깊은 복사한다.

**완료 결과.** 모든 member는 동기다. 메시지를 보내면 payload가 소비된다 — native
frame이 성공적인 send에서 transport로 옮겨지고 instance는 invalid 상태로 남는다.
보내지 않을 메시지를 해제하려면 `close()`를 호출한다.

**선택 기준.** caller가 raw 소유권을 유지할 필요가 없는 데이터로 outbound
payload를 만들 땐 크기 지정 생성자나 복사하는 `from(...)` factory를 쓴다.
`zlink::advanced::external_message_t::from(span, free_fn, hint)`(`message_t` 옆에
선언된 no-copy overload)는 caller 소유 buffer를 복사 없이 메시지에 넘겨야 할 때만
쓴다.

---

## `received_t`

수신된 message envelope 하나 — routing 메타데이터, part, 선택적 reply context를
담는다.

```cpp
zlink::received_t received;
if (dealer.recv (received) == 0) { /* ... */ }
if (received.request_seq ()) {
    received.reply ().message (reply_msg).submit ();
}
```

**옵션.** 기본 생성 가능; 복사·이동 가능.

| Member | 반환 | 의미 |
| --- | --- | --- |
| `routing_id()` | `const std::optional<routing_id_t>&` | source의 routing id, receive 경로가 제공할 때만 존재 |
| `request_seq()` | `const std::optional<uint64_t>&` | 이 envelope이 reply 가능할 때만 존재 |
| `parts()` | `const std::vector<message_t>&`(mutable overload도) | 이 envelope이 담은 모든 message part |
| `is_single_part()` | `bool` | `parts()`가 정확히 하나인지 |
| `first_part()` | `message_t` | 첫 part, 소유권 이전 없음 |
| `single_part_or_throw()` | `message_t` | 단일 part, `parts()`가 둘 이상이면 예외 |
| `send()` | builder | 이 envelope이 포착한 routing id로 향하는 공유 `send_operation_t` 시작 |
| `reply()` | builder | 공유 `reply_operation_t` 시작; 유효한 reply context가 없으면 `submit()`에서 예외 |
| `close()` | — | 이 envelope이 소유한 message part를 해제 |

**완료 결과.** 모두 동기다. `send()`/`reply()`는 저장된 routing id와 request
sequence로부터 submit 시점에 지연 생성되는 send/reply context를 재구성한다 —
server hot path에서 receive마다 `std::function` closure와 heap 할당을 피한다.

**선택 기준.** message마다 새로 생성하는 대신 receive loop 전체에서 `received_t`
하나를 재사용한다. `reply()`를 호출하기 전에 `request_seq()`로 envelope이 실제로
reply 가능한지 확인한다.

---

## `topic_message_t`

수신된 publish 하나 — topic과 message part를 담는다.

```cpp
zlink::topic_message_t published;
if (sub.subscribe (published) == 0) {
    const std::string &topic = published.topic ();
}
```

**옵션.** 기본 생성 가능, 그리고 routing id/topic/parts를 직접 받는 생성자.

| Member | 반환 | 의미 |
| --- | --- | --- |
| `routing_id()` | `const std::optional<routing_id_t>&` | 발행자의 routing id, receive 경로가 제공할 때만 존재 |
| `topic()` | `const std::string&` | 이 publish가 전송된 topic |
| `parts()` / `is_single_part()` / `first_part()` / `single_part_or_throw()` / `close()` | — | `received_t`와 같은 형태 |

**완료 결과.** 동기다.

**선택 기준.** `received_t`와 같은 방식으로 subscribe-receive loop 전체에서
instance 하나를 재사용한다.

---

## `subscription_event_t` / `subscription_filter_t`

XPUB socket이 관찰한 구독자 한 명의 subscribe·unsubscribe를 보고하고, 활성 구독
항목 하나를 기술한다.

```cpp
zlink::subscription_event_t evt;
if (xpub.receive_subscription_event (evt) == 0) { /* ... */ }
```

**옵션.**

| 타입 | Field | 의미 |
| --- | --- | --- |
| `subscription_event_t` | `routing_id`(`std::optional<routing_id_t>`) | 구독자의 routing id, receive 경로가 제공할 때만 존재 |
| | `topic`(`std::string`) | 구독·구독 취소된 topic |
| | `subscribed`(`bool`) | 구독이면 `true`, 구독 취소면 `false` |
| `subscription_filter_t` | `filter`(`std::string`) | 구독된 topic 또는 pattern 텍스트 |
| | `is_pattern`(`bool`, 기본값 `false`) | `filter`가 리터럴 topic이 아니라 pattern인지 |

**완료 결과.** 둘 다 dispose나 async 동작이 없는 순수 데이터 struct다.

**선택 기준.** XPUB socket의 subscription-event receive 경로(Sockets category)에서
구독자 변동을 관찰할 때 쓴다. socket의 `subscription_at(index)` 값 반환 overload의
반환 타입으로 `subscription_filter_t`를 쓴다.

---

## Send / request / reply operation-builder 형태

모든 `send`, routed `send`, `publish`, `request`, `reply` 진입점(전부 Sockets
category)이 part·flag·terminal submit을 누적하기 위해 반환하는 move-only fluent
builder. 이 family의 모든 builder는 공유 `detail::operation_builder_base_t`를
private 상속한다 — 그 자체는 public contract가 아니다.

```cpp
std::move (dealer.send ()).message (part1).message (part2).submit ();

auto result = std::move (dealer.request ())
    .message (request_msg)
    .timeout (std::chrono::seconds (5))
    .async ();
std::vector<zlink::message_t> reply = result.get ();

std::move (received.reply ()).message (reply_msg).submit ();
```

**옵션.**

| 단계 | Member | 의미 |
| --- | --- | --- |
| `send_operation_t` | `.message(message_t&)`/`.message(message_t&&)` | `&&`-qualified — 호출마다 builder가 소비됨, `std::move(...)`로 chain |
| `send_submit_operation_t` | `.message(...)` / `.flags(int)` / `.submit()` | part 추가, flag 설정, terminal |
| `request_operation_t`/`request_submit_operation_t` | `send`와 동일 + `.timeout(std::chrono::milliseconds)` | send chain을 그대로 반영하며 reply 대기 timeout을 더함 |
| `request_submit_operation_t.flags(int)` | `request_callback_submit_operation_t`로 좁힘 | awaitable `.async()` 경로가 사라짐 — 이후 `.submit(request_callback_t)`만 도달 가능 |
| `reply_operation_t`/`reply_submit_operation_t` | `send`와 같은 형태, `.flags(...)`는 `send_flags_t::none` 외 값이면 `submit_error_t{not_supported}` | core reply 함수가 send-flag 인자를 받지 않음 |

**완료 결과.** 모두 동기 호출이다; part는 성공적인 submit에서만 소비된다.

| Terminal | 반환 | 의미 |
| --- | --- | --- |
| `send_submit_operation_t::submit()` | `bool` | `send_flags_t::dontwait` backpressure일 때만 `false`, 그 외 실패는 `submit_error_t` |
| `reply_submit_operation_t::submit()` | `void` | 실패하면 `submit_error_t`를 던짐 |
| `request_submit_operation_t::async()` | `async_result_t<std::vector<message_t>>` | `.get()`은 reply를 block 대기, `.wait_for(...)`/`.wait_until(...)`은 timeout과 함께 poll — 둘 다 OS 스레드를 그대로 block하는 대신 내부적으로 request progress를 pump |
| `submit(request_callback_t)`(`request_submit_operation_t`/`request_callback_submit_operation_t`) | `bool` | dispatch 결과일 뿐, 실제 reply는 나중에 콜백으로 `(request_result_t, std::vector<message_t>)`가 전달됨 — 결과가 `request_result_t::ok`일 때만 벡터가 채워지며, 콜백이 각 메시지를 소유하고 반드시 `close()`해야 함 |

**선택 기준.** caller가 future를 기다릴 수 있을 땐 `.async()`를, 대신 callback
기반 완료가 필요할 땐 `.flags(...).submit(callback)`을 쓴다. 목적지를 손으로
재구성하는 대신 `received_t::reply()`/`send()`를 쓴다. `message()` overload가
`&&`-qualified이므로 항상 rvalue에서 chain한다 — lvalue builder는
`.message(...)`를 직접 호출할 수 없다.

---

[`Contracts/Messaging/`](../../../../bindings/cpp/include/zlink/Contracts/Messaging/)와
[C++ 바인딩 스펙](../../spec/cpp/README.ko.md)에서 전체 근거를 확인한다.
