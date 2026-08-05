한국어 | [English](03-sockets.en.md)

[레퍼런스 목차](README.ko.md)

# 03. Sockets

이 category는 `CommonSocketOptions`와 타입별 확장, 8개 구체 socket
struct, 공유 flag/enum 타입을 다룬다. **socket type 전체를 아우르는 공유
기반 trait이 없다** — 각 socket type은 `bind`/`connect`/`unbind`/
`disconnect`/TLS 메서드를 독립적으로 선언하는 자신만의 inherent `impl`
block을 가진 독립 struct다. PUB/SUB/XPUB/XSUB 4개 타입은 public trait이
아니라 내부 macro(`impl_pubsub_common!`)를 통해 공통 표면을 공유한다.
모든 socket의 `send`/`publish`/`request`/`reply`는 Messaging category에
문서화된 operation-builder family를 반환한다. 정확한 signature는
[`contracts/sockets/`](../../../../bindings/rust/src/contracts/sockets/)가
소유한다.

---

## 공유 socket 표면(공유 기반 trait 없음)

**Options.** 아래 모든 구체 socket type은 같은 메서드 집합을 독립적으로
선언한다. 이 메서드 전부가 `Result`를 반환한다, 대응하는 lifecycle
호출에 error 경로가 없는 다른 언어와 다르다.

| Member | 의미 |
| --- | --- |
| `close(&mut self) -> Result<(), CloseError>` | native socket을 닫는다 |
| `bind(&self, addr: &str) -> Result<(), BindError>` | 주소에서 listen을 시작 |
| `unbind(&self, addr: &str) -> Result<(), ConnectError>` | 주소에서 listen을 중단 |
| `last_endpoint(&self) -> Result<String, ConfigError>` | 실제로 해석된 bind 주소 |
| `set_tls_cert(&self, cert: &str)` / `set_tls_key(&self, key: &str)` / `set_tls_ca(&self, ca_cert: &str)` / `set_tls_hostname(&self, hostname: &str)` / `set_tls_trust_system(&self, bool)` | 개별 TLS setter — **여기 public 메서드로 존재한다**, 지금까지 다룬 다른 모든 언어가 결합된 형태만 노출하는 것과 다르다 |
| `set_tls_server(&self, cert, key, require_client_cert: bool)` | 결합된 서버측 TLS 설정, `bind` 전에 적용 |
| `set_tls_client(&self, ca_cert, hostname, trust_system: bool)` | 결합된 클라이언트측 TLS 설정, `connect` 전에 적용 |
| `connect(&self, addr: &str)` / `disconnect(&self, addr: &str)` | peer 주소로 connect/disconnect; `StreamSocket`(아래 참고)을 제외한 모든 socket type에 선언됨 |
| `disconnect_rid(&self, peer_rid: &RoutingId)` | 해당 routing id로 식별되는 peer를 disconnect; 위와 같은 예외 |

**Completion result.** 이 메서드 전부가 `Result`를 반환한다, 대응하는
lifecycle 호출에 error 경로가 없는 다른 언어와 다르다.

**선택 기준.** TLS 설정을 점진적으로 조립해야 할 때(예: 서로 다른 설정
소스에서 값이 도착할 때) 개별 `set_tls_cert`/`set_tls_key` 등 setter를
호출한다. 일반적인 한 번의 호출로 끝나는 경우엔 결합된
`set_tls_server`/`set_tls_client`를 쓴다.

---

## `CommonSocketOptions`

`socket.common_options()`로 도달하는, 모든 socket type이 공유하는 typed
option facade.

```rust
let options = socket.common_options();
options.set_send_high_water_mark(100_000)?;
options.set_linger(Duration::from_secs(1))?;
options.set_submit_retry_mode(SubmitRetryMode::LocalFailure)?;
```

**Options.** 아래 모든 getter/setter는 `Result<T, ConfigError>`를 반환한다.

| Member | 의미 |
| --- | --- |
| `linger()` / `set_linger(Duration)` | `close()`가 대기 중인 send를 flush하기 위해 기다리는 상한 |
| `send_high_water_mark()` / `set_send_high_water_mark(u64)` | outbound accounted-byte HWM; `0`은 무제한 |
| `receive_high_water_mark()` / `set_receive_high_water_mark(u64)` | inbound accounted-byte HWM; `0`은 무제한 |
| `send_timeout()` / `set_send_timeout(Duration)` | blocking send가 기다리는 상한 |
| `receive_timeout()` / `set_receive_timeout(Duration)` | blocking receive가 기다리는 상한 |
| `immediate()` / `set_immediate(bool)` | send가 지금 당장 살아있는 연결을 요구하는지, 아니면 연결이 생길 때까지 큐잉하는지 |
| `rid_duplicate_policy()` / `set_rid_duplicate_policy(RidDuplicatePolicy)` | peer가 기존 routing id를 재사용할 때 벌어지는 일 |
| `connect_timeout()` / `set_connect_timeout(Duration)` | connect handshake가 기다리는 상한 |
| `ipv6()` / `set_ipv6(bool)` | socket이 IPv6 연결을 받아들이는지 |
| `tcp_no_delay()` / `set_tcp_no_delay(bool)` | `true`면 Nagle 알고리즘을 비활성화 |
| `tcp_keepalive()` / `set_tcp_keepalive(bool)` | OS TCP keepalive 모드 — **여기선 순수 on/off `bool`이다**, 다른 언어의 tri-state -1/0/1 정수와 다름 |
| `max_message_size()` / `set_max_message_size(i64)` | 수신 허용하는 메시지 한 건의 최대 바이트 크기 |
| `backlog()` / `set_backlog(i32)` | listening socket의 대기 연결 큐 길이 |
| `reconnect_interval()` / `set_reconnect_interval(Duration)` | 재연결 시도 사이 간격 |
| `reconnect_interval_max()` / `set_reconnect_interval_max(Duration)` | 재연결 간격의 상한 |
| `submit_retry_mode()` / `set_submit_retry_mode(SubmitRetryMode)` | local back-pressure에서 실패한 submit이 자동 재시도되는지 |
| `submit_retry_timeout()` / `set_submit_retry_timeout(Duration)` | `submit_retry_mode()`가 `LocalFailure`일 때의 재시도 timeout |
| `submit_retry_attempts()` / `set_submit_retry_attempts(i32)` | `submit_retry_mode()`가 `LocalFailure`일 때의 재시도 횟수 상한 |

**Completion result.** 모든 getter/setter는 동기이며 `Result<_,
ConfigError>`를 반환한다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket이 메시지 교환을
시작하기 전에 `send_high_water_mark`/`receive_high_water_mark`,
`linger`를 설정한다.

---

## `PairSocket`

라우팅이 없는 배타적 1:1 peering socket.

```rust
let pair = ctx.pair_socket()?;
pair.send().message(Message::try_from("ping")?)?.submit()?;
let mut received = Received::empty();
if pair.recv(&mut received, RecvFlags::NONE)? { /* ... */ }
```

**Options.** 위 공유 lifecycle/TLS 표면에 더해:

| Member | 의미 |
| --- | --- |
| `send(&self) -> SendOp<Empty>` | 공유 `SendOp` builder를 시작 |
| `recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError>` | `out`을 다음 메시지로 채움 |
| `on_send_ready<F>(&mut self, handler: F) -> Result<(), HandlerError> where F: Fn() + Send + 'static` | back-pressure 해제 콜백을 등록 |
| `common_options() -> CommonSocketOptions<'_>` | 공유 option facade |

**Completion result.** `recv`는 `RecvFlags::DONT_WAIT`가 설정되고
메시지가 없을 때만 `Ok(false)`를 반환한다.

**선택 기준.** 배타적 point-to-point 링크엔 PAIR를 쓴다 — peer 라우팅이
없고 load-balance하지 않는다.

---

## `DealerSocket`

연결된 peer 전체에 send를 load-balance하고 routed request를 낼 수
있다.

```rust
let dealer = ctx.dealer_socket()?;
dealer.set_routing_id(&"worker-3".into())?;
dealer.request()
    .message(Message::try_from("payload")?)
    .submit(|result| { /* result: Result<Vec<Message>, RequestError> */ })?;
```

**Options.** `PairSocket`과 같은 공유 표면에 더해:

| Member | 의미 |
| --- | --- |
| `request(&self) -> RequestOp<Empty>` | 공유 `RequestOp` builder를 시작; target 인자 없음 — DEALER는 API 레벨 peer routing id가 없기 때문 |
| `set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError>` / `routing_id(&self) -> Result<RoutingId, ConfigError>` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `dealer_options() -> DealerSocketOptions<'_>` | 타입별 option facade 반환: `set_probe(bool)` — set-only, getter 없음; `weight()`/`set_weight(u32)`; `set_request_timeout(Duration)` — set-only, getter 없음, 이 option에 대한 다른 모든 언어의 Dealer 비대칭과 일치 |

**Completion result.** `recv`(`PairSocket`과 같은 형태)는 같은 관례를
따른다.

**선택 기준.** peer가 첫 메시지부터 이를 관찰하도록 connect 전에
`set_routing_id`를 설정한다. DEALER는 임의 token에 reply할 protocol
envelope helper가 없다 — 대신 수신된 request context
(`Received::reply()`)나 명시적 ROUTER reply 표면에서 답한다.

---

## `RouterSocket`

routing id로 지정된 peer에게 메시지를 보내고, 특정 peer의 request에
reply할 수 있다.

```rust
let router = ctx.router_socket()?;
router.send(&peer_rid).message(Message::try_from("hello")?)?.submit()?;
```

**Options.** `PairSocket`과 같은 공유 lifecycle/TLS 표면(여기서도
자신만의 사본을 독립적으로 선언한다, 다른 모든 socket type과 동일)에
더해:

| Member | 의미 |
| --- | --- |
| `send(&self, target: &RoutingId) -> SendOp<Empty>` | 공유 `SendOp`을 그 peer로 향해 시작 |
| `recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError>` | `out`을 다음 메시지로 채움 |
| `request(&self, peer_rid: &RoutingId) -> RequestOp<Empty>` | Messaging category의 `RequestOp`, 특정 peer로 향함 |
| `reply(&self, rid: &RoutingId, request_seq: u64) -> ReplyOp<Empty>` | Messaging category의 `ReplyOp`, 해당 peer의 request에 응답 |
| `on_send_ready<F>(...)` | back-pressure 해제 콜백을 등록 |
| `set_routing_id(&self, &RoutingId)` / `routing_id(&self)` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `common_options()` | 공유 option facade |
| `router_options() -> RouterSocketOptions<'_>` | 타입별 option facade 반환: `set_mandatory(bool)` — set-only; `set_probe(bool)` — set-only; `set_connect_routing_id(&RoutingId)` — set-only(**이 binding엔 할당된 connect routing id의 getter가 없다**, dotnet/cpp의 읽기 전용 `ConnectRoutingId`/`connect_routing_id()` property와 다름); `weight()`/`set_weight(u32)`; `request_timeout()`/`set_request_timeout(Duration)` — Dealer와 달리 양방향 모두 |

**Completion result.** `recv`는 `PairSocket`과 같은 관례를 따른다.
**이 binding엔 `try_send_completion_control`/
`set_completion_control_handler`가 선언돼 있지 않다** — dotnet/cpp/
java/node에서 ROUTER에 문서화된 opaque Completion-control 표면에
대응하는 게 여기엔 없다.

**선택 기준.** DEALER가 특정 peer를 지정할 수 없는 ROUTER 주도·ROUTER
응답 request/reply엔 `request(peer_rid)`/`reply(rid, request_seq)`를
쓴다.

---

## `PubSocket` / `SubSocket` / `XPubSocket` / `XSubSocket`

PUB는 매칭되는 구독자가 없으면 버리는 topic-filtered 메시지를
publish하고, SUB는 구독을 socket option으로 설정하는 방식으로
구독하며, XPUB/XSUB는 각각 구독자-event 노출과 메시지로 실어 나르는
구독을 더한다. 넷 다 public trait이 아니라 내부
`impl_pubsub_common!` macro를 통해 lifecycle/TLS 표면을 공유한다.

```rust
let pub_socket = ctx.pub_socket()?;
pub_socket.publish("prices").message(Message::try_from(tick)?)?.submit()?;

let sub = ctx.sub_socket()?;
sub.set_subscription("prices.")?;
let mut msg = TopicMessage::empty();
if sub.subscribe(&mut msg, RecvFlags::NONE)? { /* ... */ }
```

**Options.**

| 타입 | Member | 의미 |
| --- | --- | --- |
| `PubSocket` | `publish(&self, topic: &str) -> SendOp<Empty>` | 공유 `SendOp` builder를 시작; `topic`이 내부 고정 크기 검사를 통과하지 못하면 panic(`fixed_topic_or_panic`을 통해) |
| | `on_send_ready<F>(...)` | back-pressure 해제 콜백을 등록 |
| | `common_options()` | 공유 option facade |
| | `pub_options() -> PubSocketOptions<'_>` | 타입별 option facade |
| `SubSocket` / `XSubSocket` | `subscribe(&self, out: &mut TopicMessage, flags: RecvFlags) -> Result<bool, RecvError>` | `out`을 다음 매칭 publish로 채움 |
| | `set_subscription(&self, filter: &str)` / `unset_subscription(&self, filter: &str)` | topic filter를 추가/제거; 구독은 누적된다 |
| | `subscription_at(&self, index: usize) -> Result<Option<(String, bool)>, ConfigError>` | 명명된 struct/record가 아니라 `(filter, is_pattern)` tuple — 해당 index의 filter |
| | `common_options()` | 공유 option facade |
| | `sub_options() -> SubSocketOptions<'_>` | 타입별 option facade |
| `XPubSocket` | `receive_subscription_event(&self, out: &mut SubscriptionEvent, flags: RecvFlags) -> Result<bool, RecvError>` | `PubSocket` 형태 위에 이걸 더함, 자신만의 `impl` block(`PubSocket` 타입의 확장이 아님); `out`을 다음 subscribe/unsubscribe로 채움 |

**`PubSocket`도 `XPubSocket`도 `set_routing_id`/`routing_id`를 선언하지
않는다**(이 binding의 두 타입 모두에 routing-id 표면이 전혀 없다).
**`XSubSocket`의 `impl` block은 `SubSocket`의 완전하고 독립적인
사본이다** — 같은 method 집합, 같은 signature, 동일한 형태 외엔 둘을
연결하는 공유 타입이 없다.

**Completion result.** `subscribe`/`receive_subscription_event`는 위
`DONT_WAIT`에서 `Ok(false)` 관례를 따른다.

**선택 기준.** `receive_subscription_event`로 구독자 변동을 관찰하거나
`PubSocketOptions::set_manual`/`approve_subscribe`/`reject_subscribe`로
수동 admission을 하려면 특별히 `XPubSocket`을 쓴다. 구독을 일반
메시지로 실어 날라야 할 때만 특별히 `XSubSocket`을 쓴다 — 두 타입의
method 집합이 동일하므로 선택은 전적으로 어떤 생성자를 호출하는지
(`ctx.sub_socket()` vs `ctx.xsub_socket()`)에 달려 있다.

---

## `StreamSocket`

다른 모든 socket type이 쓰는 zlink wire protocol 밖에서, raw TCP peer와
framed packet을 직접 주고받는다.

```rust
let stream = ctx.stream_socket()?;
stream.on_packet(|routing_id, header, body| { /* header/body 소유 */ })?;
```

**Options.**

| Member | 의미 |
| --- | --- |
| `send(&self, target: &RoutingId) -> SendOp<Empty>` | 공유 `SendOp`을 그 peer로 향해 시작 |
| `recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError>` | `out`을 다음 packet으로 채움; 이 binding의 `recv`는 추가로 source routing id를 `out`의 send/reply context로 포착해서, 이후 `out.send()`가 packet을 보낸 쪽으로 향하게 한다 — 다른 언어에선 이렇게 명시적으로 문서화되지 않은 STREAM 고유 보강 |
| `disconnect_rid(&self, peer_rid: &RoutingId) -> Result<(), ConnectError>` | 직접 선언됨, `StreamSocket`엔 `connect`/`disconnect`가 전혀 없다 — 다른 socket type이 공유하는 connect/disconnect/disconnect_rid 삼총사를 아예 선언하지 않는다 |
| `on_packet<F>(&mut self, handler: F) -> Result<(), HandlerError> where F: Fn(RoutingId, Message, Message) + Send + 'static` | callback 기반 packet loop를 등록; handler가 `header`와 `body`를 둘 다 소유하며, 반환하면 drop됨 |
| `on_send_ready<F>(...)` | back-pressure 해제 콜백을 등록 |
| `set_routing_id(&self, &RoutingId)` / `routing_id(&self)` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `common_options()` | 공유 option facade |
| `stream_options() -> StreamSocketOptions<'_>` | 타입별 option facade: `set_notify(bool)`/`notify()` — 활성화 시 peer connect/disconnect를 application 메시지로 전달 |

**Completion result.** `recv`는 위 `DONT_WAIT`에서 `Ok(false)` 관례를
따른다.

**선택 기준.** callback 기반 packet loop엔 `on_packet`을 쓴다.

---

## 공유 flag와 enum

| 타입 | 사용처 | 값 |
|---|---|---|
| `SendFlags`(`u32`를 감싸는 tuple struct) | 모든 send/request/reply builder의 `.flags(...)` 단계(Messaging category) | `NONE`, `DONT_WAIT`(enum variant가 아니라 associated const) |
| `RecvFlags`(`u32`를 감싸는 tuple struct) | 모든 `recv`/`subscribe`/`receive_subscription_event` | `NONE`, `DONT_WAIT`(associated const) |
| `RidDuplicatePolicy` | `CommonSocketOptions::rid_duplicate_policy` | `Reject`, `Handover` |
| `SubmitRetryMode` | `CommonSocketOptions::submit_retry_mode` | `Off`, `LocalFailure` |

**선택 기준.** `SendFlags`/`RecvFlags`는 `enum` 타입이 아니라 `pub const
NONE`/`DONT_WAIT` associated constant와 `bits()` accessor를 가진
`struct`다 — 지금까지 다룬 다른 모든 언어의 flag 표현(dotnet/java의
`[Flags] enum`, cpp의 static member를 가진 class, node의 freeze된 객체
상수)과 구별되는 설계 선택이다.

---

[`contracts/sockets/`](../../../../bindings/rust/src/contracts/sockets/)와
[Rust 바인딩 스펙](../../spec/rust/README.ko.md)에서 전체 근거를 확인한다.
