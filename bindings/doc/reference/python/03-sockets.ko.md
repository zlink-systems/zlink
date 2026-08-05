한국어 | [English](03-sockets.en.md)

[레퍼런스 목차](README.ko.md)

# 03. Sockets

이 category는 `_SocketContract`(공유 기반 `Protocol`),
`CommonSocketOptions`와 타입별 확장, 8개 구체 socket `Protocol` 타입,
send/request/reply operation-builder family(Messaging이 아니라 여기
선언됨 — README 참고)를 다룬다. 정확한 signature는
[`contracts/sockets/`](../../../../bindings/python/src/zlink/contracts/sockets/)가
소유한다.

---

## `_SocketContract` 공유 기반(관례상 private)

모든 socket type이 확장하는 기반 `Protocol`: binding, disposal,
option. 앞에 underscore가 붙어 있다 — Python의 "public API 아님"
관례다, 실제로는 `socket.py`의 module-level `__getattr__`이 이
package에서 모든 구체 socket type에 도달하는 실제 경로임에도.

```python
socket.bind("tcp://*:5555")
with socket:
    ...
```

**Options.** **`unbind`도, TLS 메서드(`set_tls_server`/`set_tls_client`
등)도 전혀 없다** — 지금까지 다룬 다른 모든 언어와 달리, 이 binding의
socket 기반 contract엔 둘 다 없다.

| Member | 의미 |
| --- | --- |
| `bind(endpoint)` | 주소에서 listen을 시작 |
| `close()` | native socket을 닫는다 |
| `options` | property, 이 socket type의 typed option facade |
| `__enter__` / `__exit__` | sync context-manager만 — **여기엔 `__aenter__`/`__aexit__`가 없다**, 이 binding의 다른 모든 resource 타입이 따르는 sync·async 둘 다 패턴과 다름 |

**Completion result.** `bind`/`close`는 반환값 없이 동기다.

**선택 기준.** 아래 모든 구체 socket type이 이 Protocol을 확장하고
자신만의 `connect`/`disconnect`/send/recv 표면을 더한다.

---

## `CommonSocketOptions`와 타입별 확장

`socket.options`로 도달하는, 모든 socket type이 공유하는 typed
option facade.

```python
socket.options.send_high_water_mark = 100_000
socket.options.linger_ms = 1000
socket.options.submit_retry_mode = SubmitRetryMode.LOCAL_FAILURE
```

**Options — `CommonSocketOptions`.** 전부 순수 get/set property다.

| Member | 의미 |
| --- | --- |
| `linger_ms` | `close()`가 대기 중인 send를 flush하기 위해 기다리는 상한 |
| `send_high_water_mark` / `receive_high_water_mark` | outbound/inbound accounted-byte HWM |
| `send_timeout_ms` / `receive_timeout_ms` | blocking send/receive가 기다리는 상한 |
| `immediate` | send가 지금 당장 살아있는 연결을 요구하는지, 아니면 연결이 생길 때까지 큐잉하는지 |
| `rid_duplicate_policy` | peer가 기존 routing id를 재사용할 때 벌어지는 일 |
| `connect_timeout_ms` | connect handshake가 기다리는 상한 |
| `ipv6` | socket이 IPv6 연결을 받아들이는지 |
| `tcp_no_delay` | `True`면 Nagle 알고리즘을 비활성화 |
| `tcp_keepalive` | OS TCP keepalive 모드 |
| `max_message_size` | 수신 허용하는 메시지 한 건의 최대 바이트 크기 |
| `backlog` | listening socket의 대기 연결 큐 길이 |
| `reconnect_interval_ms` / `reconnect_interval_max_ms` | 재연결 시도 사이 간격 / 그 간격의 상한 |
| `submit_retry_mode` | local back-pressure에서 실패한 submit이 자동 재시도되는지 |
| `submit_retry_timeout_ms` / `submit_retry_attempts` | `submit_retry_mode`가 요청할 때의 재시도 timeout/횟수 상한 |
| `heartbeat_interval_ms` | idle connection에서 heartbeat ping 간격 — **지금까지 다룬 다른 어떤 언어도 이 property를 노출하지 않는다** |
| `heartbeat_ttl_ms` | heartbeat 없이 remote가 connection을 얼마나 살려두는지 — 같은 독점성 |
| `heartbeat_timeout_ms` | connection을 죽었다고 취급하기 전 heartbeat reply를 얼마나 기다리는지 — 같은 독점성 |

**Options — 타입별 확장.**

| 타입 | Member | 의미 |
| --- | --- | --- |
| `DealerSocketOptions` | `probe` | connect 시 빈 probe 전송 |
| | `weight` | load-balancing 가중치 |
| | `request_timeout_ms` | request timeout |
| `RouterSocketOptions` | `mandatory` | 알 수 없는 route에서 조용히 버리는 대신 오류 |
| | `handover` | `rid_duplicate_policy`의 편의 wrapper |
| | `probe` | connect 시 빈 probe 전송 |
| | `connect_routing_id` | 이 binding의 socket contract 전체에서 *유일한* routing-id 형태 표면 — 어떤 socket type도 자신만의 `set_routing_id`/`get_routing_id`가 없다는 README의 참고 참조 |
| | `weight` / `request_timeout_ms` | 양방향 모두 |
| `StreamSocketOptions` | `notify` | 활성화 시 peer connect/disconnect를 application 메시지로 전달 |
| `PubSocketOptions` | `verbose` / `verboser` | 중복 포함 모든 (un)subscribe 메시지를 전달 |
| | `manual` / `manual_last_value` | 구독이 `approve_subscribe`/`reject_subscribe`를 요구; `manual_last_value`는 새로 승인된 구독자에게 topic별 마지막 캐시 메시지도 재전송 |
| | `no_drop` | back-pressure에서 조용히 버리는 대신 오류 |
| | `welcome_message` | 새로 연결된 구독자 각각에게 자동 전송 |
| | `topics_count` | 읽기 전용, 활성 구독 개수 |
| | `approve_subscribe(routing_id)` / `reject_subscribe(routing_id)` | set-only, getter 없음 |
| `SubSocketOptions` | `topics_count` | 읽기 전용 — 이 socket이 가진 유일한 타입별 option |

**Completion result.** 모든 property 읽기/쓰기는 동기다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket이 메시지
교환을 시작하기 전에 `send_high_water_mark`/`receive_high_water_mark`,
`linger_ms`를 설정한다. transport 자신의 TCP keep-alive와 독립적으로
ZMTP 레벨 liveness 감지를 조정하려면 세 `heartbeat_*` property를
쓴다.

---

## `PairSocket`

라우팅이 없는 배타적 1:1 peering socket.

```python
pair = create_pair_socket(ctx)
pair.send().message(b"ping").submit()
received = create_received()
if pair.recv_into(received):
    ...
```

**Options.** `_SocketContract` 기반 표면에 더해:

| Member | 의미 |
| --- | --- |
| `connect(endpoint)` / `disconnect(endpoint)` | peer 주소로 connect/disconnect |
| `disconnect_rid(peer_rid)` | 해당 routing id로 식별되는 peer를 disconnect |
| `send()` | 공유 `SendOp` builder를 시작 |
| `recv_into(received, *, flags=0)` | `received`를 다음 메시지로 채움, `bool` 반환 |

**Completion result.** `recv_into`는 `DONT_WAIT`가 설정되고
메시지가 없을 때만 `False`를 반환한다.

**선택 기준.** 배타적 point-to-point 링크엔 PAIR를 쓴다 — peer
라우팅이 없고 load-balance하지 않는다.

---

## `DealerSocket`

연결된 peer 전체에 send를 load-balance하고 routed request를 낼 수
있다.

```python
dealer = create_dealer_socket(ctx)
dealer.request().message(b"payload").submit(callback)
```

**Options.** **`set_routing_id`/`get_routing_id`가 전혀 없다** — README
참고.

| Member | 의미 |
| --- | --- |
| `dealer_options` | property, `DealerSocketOptions` 반환 |
| `connect(endpoint)` / `disconnect(endpoint)` | peer 주소로 connect/disconnect |
| `send()` | 공유 `SendOp` builder를 시작 |
| `request()` | 공유 `RequestOp` builder를 시작; target 인자 없음 — DEALER는 API 레벨 peer routing id가 없기 때문 |
| `recv_into(received, *, flags=0)` | `received`를 다음 메시지로 채움 |

**Completion result.** `recv_into`는 `PairSocket`과 같은
`DONT_WAIT`에서 `False` 관례를 따른다.

**선택 기준.** DEALER는 임의 token에 reply할 protocol envelope
helper가 없다 — 대신 수신된 request context(`Received.reply()`,
Messaging category)나 명시적 ROUTER reply 표면에서 답한다.

---

## `RouterSocket`

routing id로 지정된 peer에게 메시지를 보내고, 특정 peer의 request에
reply할 수 있다.

```python
router = create_router_socket(ctx)
router.send(peer_rid).message(b"hello").submit()
```

**Options.** **이 binding엔 `try_send_completion_control`/
`set_completion_control_handler`가 선언돼 있지 않다** —
dotnet/cpp/java/node에서 ROUTER에 문서화된 opaque Completion-control
표면에 대응하는 게 여기엔 없다, rust에도 같은 표면이 없는 것과 일치.

| Member | 의미 |
| --- | --- |
| `router_options` | property, `RouterSocketOptions` 반환 |
| `connect(endpoint)` / `disconnect(endpoint)` | peer 주소로 connect/disconnect |
| `send(routing_id)` | 공유 `SendOp`을 그 peer로 향해 시작 |
| `request(routing_id)` | Messaging category의 공유 `RequestOp`, 특정 peer로 향함 |
| `reply(routing_id, request_seq)` | 공유 `ReplyOp`, 해당 peer의 request에 응답 |
| `recv_into(received, *, flags=0)` | `received`를 다음 메시지로 채움 |

**Completion result.** `recv_into`는 위 관례를 따른다.

**선택 기준.** DEALER가 특정 peer를 지정할 수 없는 ROUTER 주도·ROUTER
응답 request/reply엔 `request(routing_id)`/`reply(routing_id,
request_seq)`를 쓴다.

---

## `PubSocket` / `SubSocket` / `XPubSocket` / `XSubSocket`

PUB는 매칭되는 구독자가 없으면 버리는 topic-filtered 메시지를
publish하고, SUB는 구독을 socket option으로 설정하는 방식으로
구독하며, XPUB/XSUB는 각각 구독자-event 노출과 메시지로 실어 나르는
구독을 더한다.

```python
pub = create_pub_socket(ctx)
pub.publish("prices").message(tick).submit()

sub = create_sub_socket(ctx)
sub.set_subscription("prices.")
msg = create_topic_message()
if sub.subscribe_into(msg):
    ...
```

**Options.**

| 타입 | Member | 의미 |
| --- | --- | --- |
| `PubSocket` | `pub_options` | 타입별 option facade |
| | `connect(endpoint)` / `disconnect(endpoint)` | peer 주소로 connect/disconnect |
| | `publish(topic)` | 공유 `SendOp` builder를 시작 |
| `SubSocket` | `sub_options` | 타입별 option facade |
| | `connect(endpoint)` / `disconnect(endpoint)` | peer 주소로 connect/disconnect |
| | `set_subscription(topic)` / `unset_subscription(topic)` | topic filter를 추가/제거; 구독은 누적된다 |
| | `subscription_at(index)` | `(filter, is_pattern)` tuple, 또는 `None` — 해당 index의 filter |
| | `subscribe_into(topic_message, *, flags=0)` | `topic_message`를 다음 매칭 publish로 채움 |
| `XPubSocket` | `pub_options` / `connect` / `disconnect` / `publish` | `PubSocket`과 같은 형태 |
| | `receive_subscription_event_into(event, *, flags=0)` | `event`를 다음 subscribe/unsubscribe로 채움 |
| `XSubSocket` | `sub_options` / `connect` / `disconnect` / `set_subscription` / `unset_subscription` / `subscription_at` / `subscribe_into` | **`SubSocket`과 member 집합이 동일한, 완전히 독립적인 `Protocol` 선언** — 일치하는 형태 외엔 둘을 연결하는 공유 기반 타입이 없다 |

**Completion result.** `subscribe_into`/
`receive_subscription_event_into`는 위 `DONT_WAIT`에서 `False`
관례를 따른다.

**선택 기준.** `receive_subscription_event_into`로 구독자 변동을
관찰하거나 `PubSocketOptions.manual`/`approve_subscribe`/
`reject_subscribe`로 수동 admission을 하려면 특별히 `XPubSocket`을
쓴다. 구독을 일반 메시지로 실어 날라야 할 때만 특별히 `XSubSocket`을
쓴다 — 두 Protocol의 member 집합이 동일하므로 선택은 전적으로 어떤
factory를 호출하는지(`create_sub_socket` vs `create_xsub_socket`)에
달려 있다.

---

## `StreamSocket`

다른 모든 socket type이 쓰는 zlink wire protocol 밖에서, raw TCP
peer와 framed packet을 직접 주고받는다.

```python
stream = create_stream_socket(ctx)
stream.on_packet(lambda rid, header, body: ...)
```

**Options.** **이 Protocol엔 `connect`/`disconnect`가 선언돼 있지
않다** — 다른 모든 언어의 STREAM 비대칭과 일치.

| Member | 의미 |
| --- | --- |
| `stream_options` | 타입별 option facade |
| `send(routing_id)` | 공유 `SendOp`을 그 peer로 향해 시작 |
| `recv_into(received, *, flags=0)` | `received`를 다음 packet으로 채움 |
| `on_packet(handler)` | callback 기반 packet loop를 등록; handler가 header와 body 메시지를 둘 다 소유하며 background dispatch 스레드에서 실행됨 |
| `disconnect_rid(peer_rid)` | 해당 routing id로 식별되는 peer를 disconnect |

**Completion result.** `recv_into`는 위 관례를 따른다.

**선택 기준.** callback 기반 packet loop엔 `on_packet`을 쓴다.

---

## Send / request / reply operation-builder 형태

위 모든 socket type의 `send`/`publish`/`request`/`reply` 진입점이
part·flag·terminal submit을 누적하기 위해 반환하는 fluent builder.
모든 builder 단계는 공유 `_FluentMessageOp` 기반 Protocol을 확장한다.

```python
dealer.send().message(part1).message(part2).submit()

dealer.request().message(payload).timeout(5.0).submit(
    lambda result, parts: ...
)

received.reply().message(b"ok").submit()
```

**Options.**

| Stage | Member | 의미 |
| --- | --- | --- |
| `_FluentMessageOp`(공유 기반) | `message(payload)` | part 하나를 추가, chain을 시작/계속 |
| | `messages(*payloads)` | 한 호출로 여러 part 추가 — **여기선 공유 기반 Protocol에 직접 선언돼 있다**, multi-part 편의가 별도 extension method인 다른 언어와 다름 |
| | `flags(flags)` | flag 설정 |
| `SendOp extends _FluentMessageOp` | `submit()` | terminal |
| `RequestOp extends _FluentMessageOp` | `timeout(timeout)` / `submit(callback)` | reply-wait timeout을 더함; **callback 전용, 이 Protocol엔 awaitable/Future 반환 overload가 문서화돼 있지 않다**, dotnet/java/node/cpp의 async 경로가 아니라 rust의 callback 전용 request submit과 일치 |
| `RequestCallbackOp` | `timeout` / `submit(callback)` | `.flags(...)`를 호출한 후에만 도달하는 좁혀진 타입이 아니라 **별도 Protocol**로서 `RequestOp`의 형태를 그대로 반영 — `RequestOp`와 `RequestCallbackOp` 둘 다 `submit(callback)`을 직접 노출한다 |
| `ReplyOp extends _FluentMessageOp` | `submit()` | terminal |

**Completion result.** `SendOp.submit()`/`ReplyOp.submit()`은
operation 결과를 동기로 반환한다. `RequestOp`/
`RequestCallbackOp.submit(callback)`은 나중에 background dispatch
스레드에서 reply를 `callback`에 전달한다.

**선택 기준.** part마다 `.message(...)`를 chain하는 대신 한 호출로
여러 part를 추가하려면 `messages(*payloads)`를 쓴다. async/awaitable
request 경로가 없으므로, 호출부에서 `await` 어법이 필요하면
callback 안에서 `asyncio`로 직접 연결한다(`Future`/event 등).

---

## Socket enum

| Enum | 사용처 | 값 |
|---|---|---|
| `SocketType` | 내부 socket 종류 식별 | `ANY`, `PAIR`, `PUB`, `SUB`, `DEALER`, `ROUTER`, `XPUB`, `XSUB`, `STREAM` |
| `SendFlags` | 모든 send/request/reply builder의 `.flags(...)` 단계(위) | `NONE`, `DONT_WAIT` |
| `RecvFlags` | 모든 `recv_into`/`subscribe_into`/`receive_subscription_event_into` | `NONE`, `DONT_WAIT` |
| `SubmitResult` | `SubmitError`가 반영(Errors category) | `OK`, `BACKPRESSURED`, `NOT_CONNECTED`, `NOT_FOUND`, `TERMINATED`, `INVALID_HANDLE`, `INVALID_ARGUMENT`, `NOT_SUPPORTED`, `INVALID_STATE`, `THREAD_VIOLATION`, `OUT_OF_MEMORY`, `SEQ_EXHAUSTED`, `INTERNAL_ERROR`, `NOT_ADMITTED` |
| `RequestResult` | `RequestError`가 반영(Errors category) | `OK`, `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |
| `RecvResult` | `RecvError`가 반영(Errors category) | `OK`, `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206), `BUFFER_TOO_SMALL`(207), `INVALID_STATE`(208) — 더 완전한 8개 값 집합(node와 일치, dotnet/cpp/java/rust의 6개 값 집합과 다름) |
| `HandlerResult` | handler 등록 API | `OK`, `INVALID_ARGUMENT`(301), `BUSY`(302), `NOT_SUPPORTED`(303), `DEADLOCK`(304), `INVALID_HANDLE`(305), `INTERNAL_ERROR`(306) |
| `RidDuplicatePolicy` | `CommonSocketOptions.rid_duplicate_policy`, `RouterSocketOptions.handover` | `REJECT`, `HANDOVER` |
| `SubmitRetryMode` | `CommonSocketOptions.submit_retry_mode` | `OFF`, `LOCAL_FAILURE` |

**선택 기준.** 두 flags enum 어느 쪽이든 `DONT_WAIT`는 blocking 호출을
non-blocking으로 바꿔 block하는 대신 `False`/back-pressure를
보고한다.

---

[`contracts/sockets/`](../../../../bindings/python/src/zlink/contracts/sockets/)와
[Python 바인딩 스펙](../../spec/python/README.ko.md)에서 전체 근거를 확인한다.
