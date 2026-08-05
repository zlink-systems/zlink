# 03. Messaging execution — Channel messaging

[레퍼런스 목차](README.ko.md)

이 category는 `route_client_t`와 `publisher_t`가 제공하는 진입점을 다룬다. 정확한 signature는
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md)가
소유한다. 이 문서는 그 signature를 반복하지 않고, 각 진입점을 실제로 호출할 때 필요한 정보만
완결된 형태로 모은다.

Framework는 같은 의미의 typed client를 DI로 여러 개 기본 등록한다(`request_client_t`,
`message_bus_t`도 ChannelName 기반 send/request를 제공한다). 이 문서는 `.NET`의
`IZLinkRouteClient`와 가장 가까운 `route_client_t`를 기준으로 서술하며, 다른 client는 같은 완료
kind와 admission 규칙을 공유하는 대체 표면이다.

---

## `send_to_channel<TMessage>`

ChannelName에 등록된 ready target(RouteMesh 또는 ClientServer) 하나에 one-way message를 보낸다.
Reply를 기다리지 않는다.

```cpp
co_await route_client
  .send_to_channel("game.api", player_online_t{"player-1"})
  .submit();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | handler에 전달할 key-value |
| `.submit()` | 필수 terminal | source-local admission 성공까지만 기다린다 |

**완료 결과.** 정상 완료는 이 프로세스가 message를 큐에 수락했다는 뜻이다. Remote handler 실행이나
subscriber 수신은 기다리지 않는다. 큐 여유가 없으면 socket send timeout(설정하지 않으면 1초)까지
기다린 뒤 그래도 없으면 `deadline_exceeded`로 완료한다. ChannelName에 ready target이 없으면
`not_found`, route 단절은 `unavailable`, runtime 종료 중이면 `shutting_down`인
`framework_exception_t`로 완료한다.

**선택 기준.** Reply가 필요 없는 fire-and-forget에 쓴다. Reply가 필요하면 `request_to_channel`을
쓴다.

---

## `request_to_channel<TRequest>`

ChannelName 하나로 ready target을 선택해 typed request를 보내고 typed reply를 기다린다.

```cpp
player_t reply = co_await route_client
  .request_to_channel("game.api", get_player_t{"player-1"})
  .timeout(std::chrono::seconds{3})
  .submit<player_t>();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | request에만 붙는다. reply는 request metadata를 자동 복사하지 않는다 |
| `.timeout(milliseconds)` | MeshNode의 `set_default_request_timeout(...)` 값 | reply를 기다리는 상한. 전송 admission 자체는 socket send timeout이 별도로 담당한다 |
| `.submit<TReply>()` | terminal(택 1) | reply 수신까지 현재 coroutine을 이어서 기다린다 |
| `.yield<TReply>()` | terminal(택 1) | `spot_wide` User Spot·Instance Spot handler 안에서만 유효하다. 대기 동안 shared turn을 반환해 형제 job의 실행을 허용한다. 그 밖의 실행 context에서 호출하면 `invalid_operation`으로 완료한다 |

**완료 결과.** `TReply`(handler 반환값)로 완료하거나, timeout이면 `deadline_exceeded`,
ChannelName에 ready target이 없으면 `not_found`, route 단절은 `unavailable`, runtime 종료 중이면
`shutting_down`인 `framework_exception_t`로 완료한다.

**선택 기준.** Reply 값이 필요할 때 쓴다. One-way면 `send_to_channel`을 쓴다. `yield`는
`spot_wide` handler 안에서 다른 request나 worker가 진행 중일 때, 자신의 대기가 형제 job을 막지
않게 하려고 쓴다.

---

## `send_to_node<TMessage>`

MeshName과 target Node RID를 직접 지정해 one-way message를 보낸다. ChannelName 기반 선택이 아니라
특정 MeshNode 하나를 관리할 때 쓴다.

```cpp
co_await route_client
  .send_to_node("play", zlink::routing_id_t::from("play-node-1"),
                drain_requested_t{})
  .submit();
```

**옵션.** `send_to_channel`과 동일하다 — `.metadata(...)`, terminal `.submit()`.

**완료 결과.** `send_to_channel`과 같은 완료 kind를 쓴다. 대상 RID가 Object Client(handler 등록이
불가능한 RID)이면 다른 target으로 넘기지 않고 `not_found`로 완료한다.

**선택 기준.** 업무 object(actor·spot)의 배치나 메시징에는 쓰지 않는다 — 그 경우에는
ActorId·SpotId·ChannelName을 쓴다. Node direct는 운영 목적으로 특정 node를 지목할 때만 쓴다.

---

## `request_to_node<TRequest>`

MeshName과 target Node RID를 직접 지정해 typed request/reply를 주고받는다.

```cpp
node_status_t status = co_await route_client
  .request_to_node("play", zlink::routing_id_t::from("play-node-1"),
                    get_node_status_t{})
  .submit<node_status_t>();
```

**옵션.** `request_to_channel`과 동일하다 — `.metadata(...)`, `.timeout(...)`, terminal
`.submit<TReply>()` 또는 `.yield<TReply>()`.

**완료 결과와 선택 기준.** `request_to_channel`과 같되, 대상 선택이 ChannelName round-robin이
아니라 지정한 RID 고정이라는 점만 다르다.

---

## `publish<TEvent>` (classic fanout)

독립 fanout channel에 typed event를 발행한다. `route_client_t`의 channel operation과는 다른
family다 — 발행자는 구독자를 알지 못한다.

```cpp
co_await publisher
  .publish("lobby.events", player_joined_t{"player-1"})
  .submit();

// topic을 명시해야 하는 경우
co_await publisher
  .publish("lobby.events", "region.eu", player_joined_t{"player-1"})
  .submit();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| topic 인자 생략 | event의 `packet_name`을 topic으로 사용 | 예약된 topic 이름(내부 liveness용 exact byte `01 5A 4C 46 31`)을 쓰면 `framework_exception_t`로 완료한다 |
| `.submit()` | 필수 terminal | source-local publish admission 완료까지만 기다린다 |

**완료 결과.** 정상 완료는 발행 admission이 끝났다는 뜻이다. Subscriber 수나 수신 완료는 반환하지
않는다 — target이 0개여도 정상 완료한다. 시작한 뒤에는 개별 target 실패를 전체 실패로 바꾸지 않고
재시도하지 않는다.

**선택 기준.** 발행자가 구독자를 알지 못해야 하는 관찰·통지에 쓴다. 특정 대상에 보내는 메시징이면
`send_to_channel`이나 `request_to_channel`을 쓴다.

---

## Codec 등록 (구성 시점)

다른 항목과 달리 terminal이 아니라 host 구성 시점의 등록 호출이다. JSON만 쓰는 application은 이
항목을 쓸 필요가 없다.

```cpp
app.add_zlink_framework([&](auto &options) {
    options.codecs().use(protobuf_codec_extension); // 선택한 codec extension package가 제공하는 값
});
```

```cmake
# Protobuf가 필요할 때만 추가한다.
target_link_libraries(app PRIVATE zlink::framework_codec_protobuf)
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.use(extension)` | 없으면 JSON | Business payload serializer를 등록한다. 여러 번 호출해 여러 content type을 등록할 수 있다 |

**완료 결과.** 반환값 없이 동기로 등록된다. Host 시작 전에만 호출한다.

**선택 기준.** `options.codecs().use(...)`는 일반 message type을 나열하는 단계가 아니라, 기본
JSON으로 표현할 수 없는 payload나 별도 binary serializer extension을 연결하는 고급 확장점이다.
Handler와 client의 typed API는 `request_type`/`reply_type`/`message_type`/`event_type`을 handler
등록에서 읽어 자동으로 JSON serializer를 선택하므로, 모든 request/reply 타입을 codec 설정에
나열할 필요가 없다.

STREAM 연결의 wire codec은 별도 계약(`stream_compression_options_builder_t`, topology-discovery
category의 "기타 host-wide 옵션" 항목)이다. 이 항목은 business payload serializer만 다룬다.

---

## 공통 실패·취소 규칙 (모든 항목에 적용)

이 category의 모든 진입점에 공통으로 적용되며, 항목마다 반복하지 않는다.

- C++ server call에는 별도 cancellation 인자가 없다. 반환된 `task_t`를 보관하지 않거나 파괴해도
  operation이 취소된다고 보장하지 않는다.
- Admission, timeout, shutdown이 경쟁하면 원자적으로 하나만 terminal이 되고, 그 뒤 late admission을
  만들지 않는다.
- 잘못된 인자·handle·상태와 중복 `submit()`은 `framework_exception_t`로 완료하며, 이 문서가
  나열하는 완료 kind(`not_found`/`unavailable`/`deadline_exceeded`/`shutting_down`)와는 같은 예외
  타입이지만 `kind()`로 구분한다.

전체 근거는
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md)와
[Common runtime exact interface](../../common/spec/server/languages/cpp/interfaces/01-common-runtime.ko.md)를 참고한다.
