한국어 | [English](03-sockets.en.md)

[레퍼런스 목차](README.ko.md)

# 03. Sockets

이 category는 `socket_t`(모든 구체 socket type이 파생하는, public 생성이 불가능한
공유 기반), `common_socket_options_t`와 타입별 서브클래스, 8개 구체 socket type,
자유 함수 `proxy`/`proxy_steerable`를 다룬다. 모든 socket의
`send`/`publish`/`request`/`reply`는 Messaging category에 문서화된
operation-builder family를 반환한다 — 이 category는 각 builder가 어디서 시작하고
각 구체 타입이 고유하게 무엇을 더하는지만 다룬다. dotnet의 `ISocket`/
`IStreamSocket` interface와 달리 C++는 기본적으로 role interface를 노출하지 않는다
— 각 socket type은 구체 RAII class이며, `socket_t` 자체는 public 생성이 불가능하다
(생성자가 `protected`). 정확한 signature는
[`Contracts/Sockets/`](../../../../bindings/cpp/include/zlink/Contracts/Sockets/)가
소유한다.

---

## `socket_t` 공유 기반

모든 구체 socket type이 파생하는, public 생성이 불가능한 기반 — lifetime, binding,
TLS, monitoring. data-plane `send`/`receive`/`publish`는 여기선 `protected`다 —
아래 각 구체 socket type이 필요한 부분집합을 public method로 재노출한다.

```cpp
socket.bind ("tcp://*:5555");
socket.set_tls_server (cert_path, key_path, /*require_client_cert=*/true);
zlink::socket_monitor_t monitor = socket.monitor_open (zlink::monitor_event::all);
socket.close ();
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `valid()` | — | 이 socket이 아직 사용 가능한지 |
| `close()` | — | native socket을 즉시 해제 |
| `bind(const std::string&)` / `unbind(const std::string&)` | — | 주소에서 수신 시작/중지 |
| `connect(const std::string&)` / `disconnect(const std::string&)` | — | peer 주소로 connect/disconnect |
| `disconnect_rid(const routing_id_t&)` | — | 그 routing id로 식별되는 peer의 connection을 끊음 |
| `monitor_open(monitor_event events_) const` | `monitor_event::all` | `socket_monitor_t` 반환(Eventing category) |
| `options()` | — | `common_socket_options_t`(아래) 반환 |
| `set_tls_server(cert, key, require_client_cert)` | `require_client_cert = false` | `bind` 전에 적용 |
| `set_tls_client(ca_cert, hostname, trust_system)` | `trust_system = false` | `connect` 전에 적용 |
| `set_send_ready_handler(std::function<void()>)` | — | back-pressure 해소 콜백 등록 |

**완료 결과.** `valid()`/`monitor_open()`/`options()`를 제외한 모든 member는
반환값 없이 동기다. `socket_t`는 move-only다(복사는 delete) — 소멸자는 암묵적으로
close하지 않는다.

**선택 기준.** `bind`/`connect` 전에 각각 `set_tls_server`/`set_tls_client`를
호출한다. `socket_t` 자체가 아니라 구체 socket type을 직접 생성한다 — 기반
class는 public 생성자가 없다.

---

## `common_socket_options_t`와 타입별 option facade

`socket.options()`로 도달하는, 모든 socket type이 공유하는 typed option facade.

```cpp
socket.options ().send_hwm (zlink::byte_count_t::bytes (100'000));
socket.options ().linger (std::chrono::seconds (1));
socket.options ().submit_retry_mode (zlink::submit_retry_mode_t::local_failure);
```

**옵션.** `common_socket_options_t`:

| Member | 타입 | 의미 |
| --- | --- | --- |
| `linger()` | `std::chrono::milliseconds` | `close()`가 대기 중인 send가 flush될 때까지 기다리는 상한 |
| `send_hwm()` / `recv_hwm()` | `byte_count_t`, accounted-byte 제한 | send/receive queue 제한 — Core category의 byte-HWM 참고 |
| `send_timeout()` / `recv_timeout()` / `connect_timeout()` | `std::chrono::milliseconds` | 대응하는 blocking operation이 기다리는 상한 |
| `immediate()` | `bool` | send가 지금 살아있는 connection을 요구할지, 아니면 생길 때까지 대기열에 쌓을지 |
| `ipv6()` | `bool` | socket이 IPv6 connection을 받을지 |
| `tcp_no_delay()` | `bool` | `true`면 Nagle 알고리즘을 끔 |
| `tcp_keepalive()` | `tcp_keepalive_mode_t` | OS TCP keepalive 모드 |
| `rid_duplicate_policy()` | `rid_duplicate_policy_t` | peer가 기존 routing id를 재사용하면 어떻게 되는지 |
| `max_message_size()` | `byte_size_t` | 단일 수신 메시지의 최대 바이트 크기 |
| `backlog()` | `socket_backlog_t` | listening socket의 대기 connection queue 길이 |
| `reconnect_interval()` / `reconnect_interval_max()` | `std::chrono::milliseconds` | 재연결 시도 사이 간격, 그리고 그 상한 |
| `submit_retry_mode()` | `submit_retry_mode_t` | local back-pressure에서 실패한 submit을 자동 재시도할지 |
| `submit_retry_timeout()` | `std::chrono::milliseconds` | `submit_retry_mode()`가 `local_failure`일 때의 재시도 timeout |
| `submit_retry_attempts()` | `int` | `submit_retry_mode()`가 `local_failure`일 때의 재시도 횟수 상한 |
| `last_endpoint()` | `std::string`, 읽기 전용 | 실제로 resolve된 bind 주소 |

타입별 서브클래스(각각 대응하는 socket type의 참조로 생성):

| 타입 | 더하는 것 |
| --- | --- |
| `router_socket_options_t` | `mandatory()`, `handover()`, `probe()`, `connect_routing_id()`(`std::optional<routing_id_t>`), `request_timeout()`, `peer_weight()`(`peer_weight_t`) |
| `dealer_socket_options_t` | `probe()`, `request_timeout()`, `peer_weight()` |
| `stream_socket_options_t` | `notify()`(`bool`) |
| `pub_socket_options_t` | `verbose()`/`verboser()`/`no_drop()`/`manual()`/`manual_last_value()`(`bool`), `welcome_message()`(`message_t`), `approve_subscribe(const routing_id_t&)`/`reject_subscribe(const routing_id_t&)`, `topics_count()`(`int`) |
| `sub_socket_options_t` | `topics_count()`만 |

**완료 결과.** 모든 getter/setter는 동기다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket이 메시지 교환을 시작하기
전에 `send_hwm`/`recv_hwm`, `linger`를 설정한다.

---

## `pair_socket_t`

라우팅이 없는 배타적 1:1 peering socket.

```cpp
zlink::pair_socket_t pair (ctx);
std::move (pair.send ()).message (part).submit ();
zlink::received_t received;
if (pair.recv (received) == 0) { /* ... */ }
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `explicit pair_socket_t(context_t&)` | — | 그 context에 묶인 socket을 생성 |
| `send()` | — | 공유 `send_operation_t` builder 시작 |
| `recv(received_t&, recv_flags_t)` / `recv(message_t&, recv_flags_t)` | `recv_flags_t::none` | 뒤는 single-part shortcut |
| `set_send_ready_handler(std::function<void()>)` | — | back-pressure 해소 콜백 등록 |

**완료 결과.** `recv`는 `int`를 직접 반환한다 — 성공하면 `0`, receive 실패나
no data면 `recv_result_t` 값, binding-local 실패에서만 `errno`가 설정된 채
`-1`이다(dotnet의 `bool`이 아니라 이 `int` 반환 관례는 이 category의 모든 구체
socket type의 `recv`가 공유한다).

**선택 기준.** 배타적 point-to-point 링크엔 PAIR를 쓴다 — peer 라우팅이 없고
load-balance하지 않는다.

---

## `dealer_socket_t`

연결된 peer 전체에 send를 load-balance하고 routed request를 낼 수 있다.

```cpp
zlink::dealer_socket_t dealer (ctx);
dealer.set_routing_id (zlink::routing_id_t::from (std::string ("worker-3")));
auto reply = std::move (dealer.request ()).message (payload).async ().get ();
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `explicit dealer_socket_t(context_t&)` | — | 그 context에 묶인 socket을 생성 |
| `send()` / `recv(received_t&, recv_flags_t)` / `recv(message_t&, recv_flags_t)` | `recv_flags_t::none` | `pair_socket_t`와 같은 형태 |
| `set_send_ready_handler(...)` | — | back-pressure 해소 콜백 등록 |
| `request()` | — | 공유 `request_operation_t` 시작; target 인자 없음 — DEALER는 API 레벨 peer routing id가 없음 |
| `set_routing_id(const routing_id_t&)` / `get_routing_id(routing_id_t&) const` | — | 이 socket 자신의 routing id를 지정/읽음, peer가 connect 시 관찰 |
| `options()` | — | `dealer_socket_options_t` 반환 |

**완료 결과.** `recv`는 `pair_socket_t`와 같은 `int` 관례를 따른다.

**선택 기준.** peer가 첫 메시지부터 관찰하도록 connect 전에 `set_routing_id`를
설정한다. DEALER는 임의 token에 reply할 protocol envelope helper가 없다 — 수신된
request context(`received_t::reply()`)나 명시적 ROUTER reply 표면에서 답한다.

---

## `router_socket_t`

routing id로 지정된 peer에게 메시지를 보내고, 특정 peer의 request에 reply할 수
있다.

```cpp
zlink::router_socket_t router (ctx);
std::move (router.send (peer_rid)).message (part).submit ();
router.set_completion_control_handler ([] (auto &rid, auto parts) { /* ... */ });
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `explicit router_socket_t(context_t&)` | — | 그 context에 묶인 socket을 생성 |
| `send(const routing_id_t&)` | — | 그 peer로 향하는 공유 `send_operation_t` 시작 |
| `recv(received_t&, recv_flags_t)` | `recv_flags_t::none` | 다음 메시지로 envelope을 채움 |
| `recv(routing_id_t& source_rid_out_, message_t& part_out_, recv_flags_t)` | `recv_flags_t::none` | pull 기반 single-part receive; caller가 오래 사는 `received_t`를 유지해 storage를 재할당 없이 재사용 가능 |
| `set_send_ready_handler(...)` | — | back-pressure 해소 콜백 등록 |
| `request(const routing_id_t&)` | — | Messaging category의 `request_operation_t`, 특정 peer로 향함 |
| `reply(const routing_id_t&, uint64_t request_seq_)` | — | Messaging category의 `reply_operation_t`, 그 peer의 request에 응답 |
| `try_send_completion_control(const routing_id_t& peer_rid_, const std::vector<message_t>& parts_)` | — | `parts_`를 소비하지 않고 peer의 기존 connection으로 opaque control record 전송 |
| `set_completion_control_handler(completion_control_handler_t)` | — | 수신되는 completion-control record를 받는 콜백 등록; `using completion_control_handler_t = std::function<void(const routing_id_t&, std::vector<message_t>)>` — 콜백이 수신 벡터를 소유 |
| `set_routing_id(const routing_id_t&)` / `get_routing_id(routing_id_t&) const` | — | 이 socket 자신의 routing id를 지정/읽음, peer가 connect 시 관찰 |
| `options()` | — | `router_socket_options_t` 반환 |

**완료 결과.** `try_send_completion_control`은 `bool`을 반환한다 — completion
connection이 back-pressure일 때만 `false`다. `recv`는 위 `int` 관례를 따른다.

**선택 기준.** DEALER가 특정 peer를 지정할 수 없는 ROUTER 주도·ROUTER 응답
request/reply엔 `request(peer_rid)`/`reply(rid, request_seq)`를 쓴다.
application-level receive와 독립적인 opaque bounded control record엔
`try_send_completion_control`/`set_completion_control_handler`를 쓴다.

---

## `pub_socket_t` / `xpub_socket_t`

PUB는 매칭되는 구독자가 없으면 버리는 topic-filtered 메시지를 publish하고,
XPUB는 추가로 구독자의 subscribe/unsubscribe event를 노출한다. 둘 다 내부
`publisher_socket_t` 기반(그 자체는 public 생성 불가)에서 파생한다.

```cpp
zlink::pub_socket_t pub (ctx);
std::move (pub.publish ("prices")).message (tick).submit ();

zlink::xpub_socket_t xpub (ctx);
zlink::subscription_event_t evt;
if (xpub.receive_subscription_event (evt) == 0) { /* ... */ }
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `explicit pub_socket_t(context_t&)` | — | 그 context에 묶인 socket을 생성 |
| `publish(const std::string& topic_id_)` | — | 공유 `send_operation_t` 시작 |
| `set_send_ready_handler(...)` | — | back-pressure 해소 콜백 등록 |
| `options()` | — | `pub_socket_options_t` 반환 |
| `receive_subscription_event(subscription_event_t&, recv_flags_t)` | `recv_flags_t::none` | 다음 subscribe·unsubscribe로 event를 채움; `xpub_socket_t`만 |

`pub_socket_t`는 이 투영에서 `set_routing_id`/`get_routing_id`가 없다(둘 다
있는 dotnet의 `IPubSocket`과 다르다) — `xpub_socket_t`도 마찬가지다.

**완료 결과.** `receive_subscription_event`는 위와 같은 관례로 `int`를 반환한다.

**선택 기준.** `receive_subscription_event`로 구독자 변동을 관찰하거나
`pub_socket_options_t::manual()`/`approve_subscribe`/`reject_subscribe`로 수동
admission을 하려면 특별히 `xpub_socket_t`를 쓴다. 그 외엔 publish 자체는 둘이
같게 동작한다.

---

## `sub_socket_t` / `xsub_socket_t`

SUB는 구독을 socket option으로 설정하는 방식으로 topic을 구독하고, XSUB는 대신
구독을 메시지로 실어 나른다. 둘 다 내부 `subscriber_socket_t` 기반에서
파생한다 — 하지만 각 구체 타입이 다른 signature로 자신만의 public overload를
다시 선언하므로, 기반의 형태는 caller가 직접 쓰는 public contract가 아니라
내부 배관으로 취급한다.

```cpp
zlink::sub_socket_t sub (ctx);
sub.set_subscription ("prices.");
zlink::topic_message_t msg;
if (sub.subscribe (msg) == 0) { /* ... */ }
```

**옵션.** `sub_socket_t`:

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `explicit sub_socket_t(context_t&)` | — | 그 context에 묶인 socket을 생성 |
| `set_subscription(const std::string&)` / `unset_subscription(const std::string&)` | — | topic filter를 추가/제거; 구독은 누적된다; `void` 반환, base의 `[[nodiscard]] int`와 다름 |
| `subscription_at(size_t, std::string&, bool* = nullptr)` | — | 해당 index의 filter를 출력 인자에 씀 |
| `subscription_at(size_t)` | — | 값 반환 overload, `subscription_filter_t` 반환 |
| `subscribe(topic_message_t&, recv_flags_t)` | `recv_flags_t::none` | 다음 매칭 publish로 envelope을 채움; `int` 반환, base의 예외를 던지는 값 반환 형태가 아님 |
| `subscribe_part(std::optional<routing_id_t>& source_rid_out_, std::string& topic_out_, message_t& part_out_, bool& has_more_out_, recv_flags_t)` | `recv_flags_t::none` | pull 기반 single-part subscribe receive |
| `options()` | — | `sub_socket_options_t` 반환 |

`xsub_socket_t`는 `sub_socket_t`와 member 집합이 동일하다 — 모든 method가 변경
없이 상속되는 게 아니라 각각 따로 재선언돼 있지만, SUB/XSUB 자체가 뜻하는 것
이상으로 둘 사이에 동작이 다른 member는 없다.

**완료 결과.** `subscribe`/`subscribe_part`는 위와 같은 관례로 `int`를
반환한다.

**선택 기준.** 일반적인 경우엔 `sub_socket_t`를 쓴다. 구독을 일반 메시지로
실어 날라야 할 때만 `xsub_socket_t`를 쓴다.

---

## `stream_socket_t`

다른 모든 socket type이 쓰는 zlink wire protocol 밖에서, raw TCP peer와 framed
packet을 직접 주고받는다.

```cpp
zlink::stream_socket_t stream (ctx);
stream.set_packet_handler ([] (auto &rid, auto &&header, auto &&body) { /* header/body 소유 */ });
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `explicit stream_socket_t(context_t&)` | — | 그 context에 묶인 socket을 생성 |
| `send(const routing_id_t&)` | — | 그 peer로 향하는 공유 `send_operation_t` 시작 |
| `recv(received_t&, recv_flags_t)` | `recv_flags_t::none` | 다음 packet으로 envelope을 채움; dotnet의 `IStreamSocket`(raw part와 routing id/`hasMore`를 반환하는 `RecvPart`가 있음)과 달리, 여기 public으로 선언된 별도 raw-part receive overload는 없음 |
| `set_packet_handler(std::function<void(const routing_id_t&, message_t&&, message_t&&)>)` | — | callback 기반 packet loop 등록 |
| `set_send_ready_handler(...)` | — | back-pressure 해소 콜백 등록 |
| `set_routing_id(const routing_id_t&)` / `get_routing_id(routing_id_t&) const` | — | 이 socket 자신의 routing id를 지정/읽음, peer가 connect 시 관찰 |
| `options()` | — | `stream_socket_options_t` 반환 |

**완료 결과.** `recv`는 위 `int` 관례를 따른다. packet handler는 rvalue 참조를
통해 메시지 소유권을 콜백으로 이전한다.

**선택 기준.** callback 기반 packet loop엔 `set_packet_handler`를 쓴다.

---

## `proxy` / `proxy_steerable`

두 socket 사이의 양방향 message-forwarding loop을 실행한다(선택적으로 control
socket을 통해 조종 가능). socket method가 아니라 자유 함수다 — static facade가
아니라(dotnet의 `Zlink.Proxy(...)`와 다르게) 이 category에서 `socket_t` 옆에
선언된다.

```cpp
zlink::proxy (frontend, backend);
zlink::proxy (frontend, backend, capture);
zlink::proxy_steerable (frontend, backend, capture, control);
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `proxy(socket_t& frontend_, socket_t& backend_)` | context가 종료될 때까지 두 socket 사이에 메시지를 전달 |
| `proxy(socket_t&, socket_t&, socket_t& capture_)` | 같지만, 전달되는 모든 메시지의 복사본을 `capture_`로도 보냄 |
| `proxy_steerable(socket_t&, socket_t&, socket_t& capture_, socket_t& control_)` | 같지만, `control_`의 명령으로 일시정지/재개/종료 가능 |

**완료 결과.** 둘 다 context가 종료될 때까지(또는 `proxy_steerable`의 경우
control 명령이나 에러가 loop을 끝낼 때까지) 호출 스레드를 block한다 — 둘 중
하나를 전용 스레드에서 실행한다.

**선택 기준.** 단순한 fire-and-forget forwarding loop엔 `proxy`를,
application이 다른 스레드에서 control socket을 통해 loop을 일시정지·재개·종료해야
할 땐 `proxy_steerable`을 쓴다.

---

## Socket enum과 flag

위 모든 항목에서 참조하는 공유 타입.

| 타입 | 사용처 | 값 |
|---|---|---|
| `socket_type` | 내부 socket 종류 식별 | `any`, `pair`, `pub`, `sub`, `dealer`, `router`, `xpub`, `xsub`, `stream` |
| `rid_duplicate_policy_t` | `common_socket_options_t::rid_duplicate_policy`, `router_socket_options_t::handover` | `reject`, `handover` |
| `submit_retry_mode_t` | `common_socket_options_t::submit_retry_mode` | `off`, `local_failure` |
| `tcp_keepalive_mode_t` | `common_socket_options_t::tcp_keepalive` | `os_default`, `off`, `on` |
| `send_flags_t`(static member를 가진 class, `enum`이 아님) | 모든 send/request/reply builder의 `.flags(int)` 단계(Messaging category) | `none`, `dontwait` |
| `recv_flags_t`(static member를 가진 class, `enum`이 아님) | 모든 `recv`/`subscribe`/`receive_subscription_event` | `none`, `dontwait` |
| `send_result_t` | non-blocking send 시도의 결과 | `sent`, `backpressured`, `not_ready` |
| `submit_result_t` | `submit_error_t`로 던져짐(Errors category) | `zlink_submit_result_t`를 반영(Errors category 참고) |
| `recv_result_t` | `recv`-family 호출이 실패·no-data 시 반환하는 `int` | `ok`, `no_data`(201), `busy`(202), `terminated`(203), `invalid_handle`(204), `not_supported`(205), `internal_error`(206) |

**선택 기준.** `send_flags_t`/`recv_flags_t`는 scoped `enum class` 타입이 아니라
`int`를 감싸는 class다 — `static const` member(`send_flags_t::dontwait`)를
쓴다, dotnet의 `[Flags] enum SendFlags`와 다르다. 둘 중 어느 쪽이든 `dontwait`는
blocking 호출을 non-blocking으로 바꿔 block하는 대신 back-pressure/no-data를
보고한다.

---

[`Contracts/Sockets/`](../../../../bindings/cpp/include/zlink/Contracts/Sockets/)와
[C++ 바인딩 스펙](../../spec/cpp/README.ko.md)에서 전체 근거를 확인한다.
