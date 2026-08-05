# 06. Stream session

[레퍼런스 목차](README.ko.md)

이 category는 STREAM session 코드 안에서 쓰는 진입점(`packet_stream_session_t`, `stream_t`,
`session_actor_manager_t`, `session_actor_t`)과 Actor 코드 안에서 bound session에 쓰는 진입점
(`bound_session_t`)을 다룬다. 정확한 signature는
[STREAM session exact interface](../../common/spec/server/languages/cpp/interfaces/06-stream-session.ko.md)와
[Actor exact interface](../../common/spec/server/languages/cpp/interfaces/05-actors.ko.md)가 소유한다.

---

## Session 콜백 구현 (`packet_stream_session_t`)

이 STREAM session이 받을 lifecycle·packet 이벤트를 처리한다. C++에는 assembly reflection이
없으므로 `.NET`처럼 packet type별 handler class를 등록하지 않는다 — `register_session<TSession>()`
(topology-discovery category)로 등록한 `TSession`이 직접 override하는 virtual member다.

```cpp
class game_session_t : public zlink::framework::packet_stream_session_t {
public:
    zlink::framework::task_t<void> on_connected(
      zlink::framework::stream_t &stream) override;
    zlink::framework::task_t<void> on_disconnected(
      zlink::framework::stream_t &stream) override;
    zlink::framework::task_t<void> on_error(
      zlink::framework::stream_t &stream,
      const zlink::framework::stream_error_t &error) override;
    zlink::framework::task_t<void> on_packet(
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &context,
      const zlink::framework::message_t &payload) override;
};
```

**옵션.** `on_packet(...)`의 `context.packet_name`으로 어떤 packet인지 구분한다 — 별도 registry
호출 없이 override 하나가 이 session이 받는 모든 packet을 처리한다.

**완료 결과.** 네 callback 모두 `task_t<void>`를 반환한다. `on_connected`/`on_disconnected`는
연결·해제마다 한 번, `on_error`는 transport 오류마다, `on_packet`은 Framework 내부 recv loop가
header framing과 queue admission을 끝낸 뒤 packet마다 호출한다. Handshake 실패는 session이
만들어지기 전이므로 `on_error`가 아니라 runtime monitoring에만 기록된다.

**선택 기준.** `stream-session` topology를 쓰는 모든 host가 구현한다. `context.can_reply`가
`true`인 packet에만 `reply_packet`으로 응답할 수 있다.

---

## `write_packet` (stream_t)

연결된 client에 one-way message를 보낸다.

```cpp
co_await stream
  .write_packet(zlink::framework::message_t::from(server_tick_t{tick_number}))
  .submit();
```

**옵션.** `stream_send_call_t`가 제공하는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | client에 전달할 key-value |
| `.packet_name(name)` | payload 타입의 `packet_name` | 이 packet의 이름을 명시적으로 지정 |
| `.compress()` | 비압축 | 등록된 stream compression codec으로 payload를 압축 |
| `.submit()` | 필수 terminal | source-local admission까지만 기다린다 |

**완료 결과.** messaging-execution category의 one-way 완료 kind와 같다 — socket send timeout까지
기다린 뒤 없으면 `deadline_exceeded`, connection 단절은 `unavailable`인 `framework_exception_t`로
완료한다.

**선택 기준.** Client가 보낸 request가 아닌, server가 먼저 보내는 push 메시지에 쓴다. Client의
request에 답할 때는 `reply_packet`을 쓴다.

---

## `reply_packet` (stream_t)

현재 처리 중인 request packet에 응답한다.

```cpp
co_await stream
  .reply_packet(zlink::framework::message_t::from(get_player_state_result_t{state}))
  .submit();
```

**옵션.** `stream_write_call_t`가 제공하는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | client에 전달할 key-value |
| `.compress()` | 비압축 | 등록된 stream compression codec으로 payload를 압축 |
| `.submit()` | 필수 terminal | source-local admission까지만 기다린다 |

**완료 결과.** 이 request의 one-shot reply token을 원자적으로 claim한 뒤 전송한다. 같은 token으로
만든 두 번째 `reply_packet` 호출은 claim에 실패해 transport를 시도하지 않고 exceptional
completion으로 끝난다. Caller의 request timeout은 wire로 전달되지 않으므로 이 reply의 admission
deadline은 STREAM socket send timeout만 사용한다. Timeout 뒤에는 late reply를 보내지 않는다.

**선택 기준.** `session_message_context_t::can_reply`가 `true`인 packet(request)에만 쓴다. Client가
보낸 것이 아닌 새 메시지를 보내려면 `write_packet`을 쓴다.

---

## `bind` / `bind_or_get` (session_actor_manager_t)

이 STREAM session에 Actor를 묶어 Actor 쪽에서 이 연결로 push할 수 있게 한다. `stream_t::actors()`로
호출한다.

```cpp
zlink::framework::session_actor_t bound =
  co_await stream.actors().bind_or_get(actor_ref).submit<zlink::framework::session_actor_t>();
```

**옵션.** 이 호출에는 modifier가 없다 — `actor_ref_t`만 받는다. 반환하는 `request_call_t<session_actor_t>`는
messaging-execution category의 request call과 같은 `.timeout(...)`/terminal 모양을 쓴다.

**완료 결과.** `bind`는 매번 새 binding을 만든다. `bind_or_get`은 이미 bound된 같은 incarnation이
있으면 그것을 반환한다. Binding은 `actor_id + object_generation`의 exact incarnation 하나로
고정된다. Mapping이 없으면 `not_found`, generation이 다르면 `invalid_operation`, pre-commit seal
중이면 `unavailable`이다. `find(actor_id)`로 이미 bound된 handle을 동기 조회할 수 있고,
`bound()`는 현재 session에 bound된 전체 목록을 반환한다.

**선택 기준.** Actor가 이 client 연결로 직접 push해야 할 때 bind한다. Relocation이 일어나도
`session_actor_t::ref()`가 current location snapshot으로 갱신되므로 application이 다시 bind할
필요는 없다.

---

## `relay` / `notify_disconnected` (session_actor_t)

Bind로 얻은 `session_actor_t`를 통해 이 Actor 쪽에서 client로 payload를 전달하거나 연결 단절을
통지한다.

```cpp
co_await bound.relay(zlink::framework::message_t::from(room_updated_t{state}));
```

**옵션.** 두 호출 모두 modifier가 없다 — payload(`relay`)만 받는다. `relay`는
`session_message_context_t`를 함께 받는 overload도 있다.

**완료 결과.** `relay`는 source-local admission을 수락하면 정상 완료하는 one-way `task_t<void>`
operation이다. `notify_disconnected`는 connection이 유지된 상태에서 논리적 단절을 알리는
notification이며 callback terminal까지 기다린다. Physical disconnect는 Framework가 자동으로
현재 binding 전체에 통지하므로 이 호출이 그 대체 경로는 아니다.

**선택 기준.** Actor 쪽 코드에서 특정 bound client에 직접 전달할 때 쓴다. Request에 대한 응답은
Session 쪽 `reply_packet`이 처리한다.

---

## `send` (bound_session_t, Actor 코드 안)

Actor에서 자신에게 bind된 client로 one-way message를 보낸다. `actor_context_t::bound_session()`이
반환하는 `bound_session_t`로 호출한다.

```cpp
co_await context_.bound_session()
  .send(inventory_changed_t{item})
  .submit();
```

**옵션.** `bound_session_send_call_t`가 제공하는 `.metadata(...)`와 필수 terminal `.submit()`이
있다.

**완료 결과.** messaging-execution category의 one-way 완료 kind와 같다. 이 표면은 client를 향한
새 request operation을 제공하지 않는다 — client request에 대한 reply는 Actor request handler의
반환값으로 처리한다.

**선택 기준.** Actor 코드 쪽에서 bound client로 push할 때 쓴다. Session 쪽에서 직접 보내려면 위
`write_packet` 항목을 쓴다. 연결을 끊으려면 `bound_session_t::disconnect()`를 쓴다.

---

## `close` (연결 종료)

Session이나 raw transport handle을 닫는다. `stream_t::close()`가 제공한다.

```cpp
co_await stream.close();
```

**옵션.** 이 호출에는 modifier가 없다.

**완료 결과.** 연결을 닫는다. 이미 닫힌 연결에 다시 호출해도 이 문서가 정의하는 별도 예외 계약은
없다 — 정확한 재호출 의미는 exact interface를 확인한다.

**선택 기준.** Application이 자발적으로 이 STREAM 연결을 끊어야 할 때 쓴다. Actor 쪽에서 bound
client 연결을 끊으려면 `bound_session_t::disconnect()`를 쓴다.

---

## `disconnect` (Actor 코드 안, bound session)

Actor에서 자신에게 bind된 client 연결을 끊는다. `bound_session_t::disconnect()`로 호출한다.

```cpp
co_await context_.bound_session().disconnect();
```

**옵션.** 이 호출에는 modifier가 없다.

**완료 결과.** Bound session과의 연결을 끊는다.

**선택 기준.** Actor 쪽 코드에서 특정 client 연결을 더 유지할 필요가 없을 때 쓴다. Session 쪽에서
직접 끊으려면 `close` 항목을 쓴다.

---

전체 근거는
[STREAM session exact interface](../../common/spec/server/languages/cpp/interfaces/06-stream-session.ko.md)와
[Actor exact interface](../../common/spec/server/languages/cpp/interfaces/05-actors.ko.md)를
참고한다.
