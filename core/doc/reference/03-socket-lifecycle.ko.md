한국어 | [English](03-socket-lifecycle.en.md)

[레퍼런스 목차](README.ko.md)

# 03. Socket lifecycle

이 category는 모든 raw socket 타입에 공통인 진입점 — 생성, endpoint lifecycle
(bind/connect/disconnect), 종료, send-ready callback을 다룬다. Socket 타입별 옵션과
data-plane 연산은 각자의 category(아래 PAIR/PUB/SUB/XPUB/XSUB/DEALER/ROUTER/STREAM)에
있다. 정확한 signature는 [Socket 공통 스펙](../spec/core/socket/README.ko.md)이 소유한다.

---

## `zlink_socket`

Context 안에 새 socket을 만든다. 이 category와 socket 타입별 category의 다른 모든 항목의
전제 조건이다.

```c
void *s = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
```

**Parameters.** `context_`는 `zlink_ctx_new`로 만든 context다. `type_`은
`ZLINK_SOCKET_*` 상수(`PAIR`/`PUB`/`SUB`/`DEALER`/`ROUTER`/`XPUB`/`XSUB`/`STREAM`) 중
하나다 — `ZLINK_SOCKET_ANY`는 filter API용 wildcard이며 그 자체로는 생성 대상이 아니다.

**Return과 errno.** 성공하면 socket handle을, 실패하면 `NULL`을 반환하며 `errno`가
설정된다 — 잘못된 type이면 `EINVAL`, 최대 socket 수에 도달했으면 `EMFILE`, context가
종료됐으면 `ETERM`.

**선택 기준.** Application이 필요로 하는 socket마다 한 번 호출한다. 수신 모드는 `type_`으로
고정된다 — `PAIR`/`DEALER`/`SUB`/`XSUB`는 part receive를, `ROUTER`는
`zlink_router_recv_part()`를 쓰고, `STREAM`만 raw part receive·raw callback·packet
callback 중 선택할 수 있다(STREAM category 참고). Socket은 context가 종료되기 전에
`zlink_close`로 닫아야 한다.

---

## `zlink_close`

Socket을 닫고 자원을 해제한다.

```c
zlink_close_result_t result = zlink_close(s);
```

**Parameters.** Socket handle만 받는다.

**Return과 errno.** `zlink_close_result_t`를 반환한다 — 성공하면 `ZLINK_CLOSE_OK`. Handle이
유효한 socket이 아니면 `ENOTSOCK`, 다른 스레드에서 이 handle에 callback이나 operation이
진행 중이면 `EBUSY`.

**선택 기준.** 더 이상 필요 없는 socket을 해제할 때 호출한다. 남아 있는 send queue 메시지는
`ZLINK_OPT_LINGER`(Socket options and identity category)에 따라 버려지거나 전송된다. 공개
handle은 계층화된 스레드 안전성 계약을 따른다 — hot-path `send`는 동시 호출 가능하고,
control-path 호출(bind/connect/option/monitor)은 정합성을 위해 직렬화되며, `close`는 더
엄격한 fail-fast gate를 쓴다 — 일단 수락되면 같은 handle의 새 API 진입은 `ESHUTDOWN`으로
실패한다. send-ready나 monitor callback 안에서의 self-close는 실패하지 않고 callback이
반환될 때까지 미뤄진다.

---

## `zlink_bind` / `zlink_unbind`

다른 쪽이 연결할 수 있도록 socket을 local endpoint에 bind하거나, 이전에 만든 binding을
제거한다.

```c
zlink_bind_result_t bound = zlink_bind(s, "tcp://*:5555");
// ...
zlink_unbind(s, "tcp://*:5555");
```

**Parameters.** `addr_`는 `transport://address` 형식이다 — 지원하는 transport는
`tcp://`, `inproc://`(프로세스 내부), `ipc://`(POSIX 전용), `ws://`(WebSocket),
`tls://`(TLS 암호화 TCP)다. Socket은 여러 endpoint에 bind할 수 있다. TCP에서 port `0`은
ephemeral port를 요청한다.

**Return과 errno.** `bind`는 `zlink_bind_result_t`를 반환한다 — 성공하면 `ZLINK_BIND_OK`,
주소가 이미 사용 중이면 `EADDRINUSE`, interface가 없으면 `EADDRNOTAVAIL`, 지원하지 않는
transport면 `EPROTONOSUPPORT`. `unbind`는 `zlink_connect_result_t`를 반환한다 — 성공하면
`ZLINK_CONNECT_OK`.

**선택 기준.** 연결을 수신하는 쪽에 `bind`를 쓴다. Ephemeral port(`0`)로 bind했으면 실제 bind된
endpoint를 `ZLINK_OPT_LAST_ENDPOINT`(`zlink_get_option`, Socket options and identity
category)로 조회한다. Socket을 닫지 않고 특정 endpoint의 수신만 멈추려면 `unbind`를 쓴다.

---

## `zlink_connect` / `zlink_disconnect` / `zlink_disconnect_rid`

Socket을 remote endpoint에 연결하거나, endpoint 또는 peer routing ID로 연결을 제거한다.

```c
zlink_connect(s, "tcp://peer-host:5555");
// ...
zlink_disconnect(s, "tcp://peer-host:5555");
// endpoint 대신 routing id로 제거하는 경우:
zlink_disconnect_rid(s, &peer_rid);
```

**Parameters.** `connect_`/`disconnect_`는 `bind`와 같은 endpoint 형식을 받는다.
`disconnect_rid`는 `zlink_routing_id_t *peer_rid_`를 받으며 비어 있으면 안 된다.

**Return과 errno.** 셋 다 `zlink_connect_result_t`를 반환한다 — 성공하면
`ZLINK_CONNECT_OK`. `disconnect_rid`는 추가로 대상이 없으면 `ZLINK_CONNECT_NOT_FOUND`, 중복
routing ID로 모호하면 `ZLINK_CONNECT_CONFLICT`, lifecycle 소유권 충돌이면
`ZLINK_CONNECT_BUSY`로 매핑한다.

**선택 기준.** 연결하는 쪽에 `connect`를 쓴다 — socket은 여러 endpoint에 연결할 수 있고,
peer를 쓸 수 없게 되면 library가 자동으로 재연결한다. Endpoint 주소를 알면 `disconnect`를
쓴다. Peer의 routing ID만 알면 대신 `disconnect_rid`를 쓴다 — `ROUTER`와 `STREAM`은 자신의
routing map에서 직접 찾고(`STREAM`은 `peer_rid_`가 4바이트 connection routing ID여야 함),
나머지 socket 타입은 현재 연결된 pipe의 스냅샷을 훑으며, 같은 routing ID를 가진 pipe가
둘 이상이면 실패한다. `disconnect_rid`가 성공을 반환해도 remote peer가 이미 종료를 처리했다는
뜻은 아니다.

---

## `zlink_send_ready_handler`

Send 가능한 socket이 backpressure를 벗어나 재시도할 가치가 있을 때 호출되는 callback을
설치하거나 교체한다.

```c
zlink_send_ready_handler(s, on_send_ready, userdata);
```

**Parameters.** `handler_`는 `zlink_send_ready_handler_fn`이다 — `NULL`을 넘기면 안 된다
(이 호출은 교체 전용이지 해제가 아니다). `userdata_`는 callback에 그대로 전달된다.

**Return과 errno.** `zlink_handler_result_t`를 반환한다 — 성공하면 `ZLINK_HANDLER_OK`.
지원하지 않는 socket 타입이면 `ENOTSUP`. 같은 handle의 send-ready callback 안에서 재진입
호출하면 `EDEADLK`.

**선택 기준.** Raw `PAIR`, `PUB`, `XPUB`, `DEALER`, `ROUTER`, `STREAM`에서 지원한다. 이
callback은 `ZLINK_POLLOUT`(Polling and pollers category)과 같은 send-recovery readiness
축을 공유한다 — send가 `BACKPRESSURED`를 반환한 뒤, 이 신호는 재시도할 가치가 있다는 뜻이지
재시도가 성공한다는 보장은 아니다 — 바로 다음 재시도도 여전히 `BACKPRESSURED`를 반환할 수
있다. 교체 성공은 다음 writable 전이부터 반영된다.

---

전체 근거는 [Socket 공통 스펙](../spec/core/socket/README.ko.md)을 참고한다.
