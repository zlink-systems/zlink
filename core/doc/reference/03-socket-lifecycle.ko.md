
[레퍼런스 목차](README.ko.md)

# 03. Socket lifecycle

이 category는 모든 raw socket 타입에 공통인 진입점 — 생성, endpoint lifecycle
(bind/connect/disconnect), 종료, 비동기 send, 수신 flow control을 다룬다. Socket 타입별
옵션과 data-plane 연산은 각자의 category(아래 PAIR/PUB/SUB/XPUB/XSUB/DEALER/ROUTER/STREAM)에
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
실패한다. send-completion이나 monitor callback 안에서의 self-close는 실패하지 않고 callback이
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

## `zlink_send_async` / `zlink_send_complete_handler` / `zlink_send_async_cancel`

완결된 multipart record 하나를 Core의 비동기 send에 넘기고, pending send가 어떻게 끝났는지
보고하는 callback을 설치·교체하며, 아직 완료되지 않은 operation 하나의 취소를 요청한다.

```c
zlink_send_complete_handler(s, on_send_complete, userdata);

zlink_send_op_id_t op_id = 0;
zlink_send_async_options_t opts = { .struct_size = sizeof(opts), .timeout_ms = 1000 };
zlink_submit_result_t r = zlink_send_async(s, parts, part_count, &opts, &op_id);
if (r == ZLINK_SUBMIT_OK && op_id != 0) {
    // op_id는 완료가 아직 pending인 동안에만 0이 아니다.
    zlink_send_async_cancel(s, op_id);
}
```

**Parameters.** `zlink_send_async`의 `parts_`/`part_count_`는 multipart record다.
`options_`(`zlink_send_async_options_t`, `struct_size`는 `sizeof`로 설정)는 operation별
`timeout_ms`(`0` = deadline 없음, `ZLINK_OPT_SNDTIMEO`와 무관), completion event로 그대로
돌아오는 opaque `userdata`, 이전에 스냅샷한 routed pipe를 가리키는 선택적 `target`
(`zlink_routed_submit_target_t`)을 담는다. `op_id_out_`은 caller가 즉시 admission과 pending
완료를 구분할 필요가 없으면 `NULL`일 수 있다. `zlink_send_complete_handler`의 `handler_`는
`zlink_send_complete_handler_fn`이다 — `NULL`을 넘기면 안 된다(이 호출은 교체 전용이지
해제가 아니다). `zlink_send_async_cancel`은 이전 `zlink_send_async` 호출이 반환한 `op_id`를
받는다.

**Return과 errno.** `zlink_send_async`는 `zlink_submit_result_t`를 반환한다 —
`*op_id_out_ == 0`인 `ZLINK_SUBMIT_OK`는 즉시 admission을 뜻하며 completion callback이
실행되지 않는다. `op_id`가 0이 아닌 `ZLINK_SUBMIT_OK`는 record가 pending 상태이며 정확히 한
번 완료를 받는다는 뜻이다. 설정된 `ZLINK_OPT_SEND_PENDING_MAX_MSGS`/`_BYTES` 한도를 넘으면
`ZLINK_SUBMIT_BACKPRESSURED`(part 소유권은 caller에 남는다). Completion handler를 먼저
설치하지 않았거나(호출이 pending 상태가 될 수 있으므로 handler가 필수다) subject가
`zlink_send_async`가 지원하지 않는 것이면 `EINVAL`. Raw `PAIR`/`DEALER`/`ROUTER`/`STREAM`
밖의 socket 타입이면 `ENOTSUP`. `zlink_send_complete_handler`는 `zlink_handler_result_t`를
반환한다 — 성공하면 `ZLINK_HANDLER_OK`, 같은 집합 밖의 socket 타입이면 `ENOTSUP`, 같은
handle의 completion callback 안에서 재진입 호출하면 `EDEADLK`. `zlink_send_async_cancel`은
`zlink_submit_result_t`를 반환한다 — `ZLINK_SUBMIT_OK`는 취소가 수락됐고 완료가
`ZLINK_SEND_TERMINAL`/`ECANCELED`를 보고할 것이라는 뜻이다. 그 id를 가진 pending operation이
없으면 `ZLINK_SUBMIT_NOT_FOUND`. 다른 resolver가 이미 operation을 선점했으면
`ZLINK_SUBMIT_INVALID_STATE`(그 resolver가 여전히 정확히 한 번 완료를 보고한다).

**선택 기준.** `ZLINK_SUBMIT_OK`에서 `parts_[0 .. part_count_)`의 모든 항목 소유권이
Core로 넘어간다 — caller는 close를 포함해 그 메시지를 다시 건드리면 안 된다. 다른 결과에서는
소유권이 caller에 남는다. Completion event(`zlink_send_complete_event_t` — `op_id`,
`userdata`, `peer_rid`, transport-pair identity, `result`, `terminal_errno`를 담음)의
`ZLINK_SEND_ADMITTED`는 Core send queue로의 admission을 뜻하지 peer 전달 확인이 아니다 —
전달 확인이 필요하면 request/reply를 쓴다. 한 socket의 completion은 서로 동시에 실행되지
않지만, 고정된 스레드는 보장되지 않는다 — callback은 Core async mailbox 스레드, timeout이면
Core deadline 스레드, close 중이면 closing 스레드, 또는 socket이 poller에
`ZLINK_POLLCOMPLETION`으로 등록돼 있으면 `zlink_poller_wait`를 호출한 스레드(Polling and
pollers category)에서 실행될 수 있다 — 이는 dispatch 위치만의 변경이다. Callback은 완료를
application 상태에 넘기는 일만 해야 한다 — 그 안에서 어떤 socket의 send·publish·request
진입점을 호출해도 `EDEADLK`로 실패하며, 어떤 socket의 send-completion handler를 교체해도
마찬가지다. 취소된 operation도 정확히 한 번은 완료된다.

---

## `zlink_socket_set_receive_flow_state`

이 socket의 local receive-flow state(`ZLINK_RECEIVE_FLOW_RUNNING` /
`ZLINK_RECEIVE_FLOW_PAUSED`)를 설정하고 짝을 이루는 `DEALER`/`ROUTER` completion lane에
동기화한다.

```c
zlink_config_result_t r =
  zlink_socket_set_receive_flow_state(s, ZLINK_RECEIVE_FLOW_PAUSED);
```

**Parameters.** `handle_`은 socket이다. `state_`는 `zlink_receive_flow_state_t`다
(`RUNNING = 0`, `PAUSED = 1`).

**Return과 errno.** `zlink_config_result_t`를 반환한다 — 성공하면 `ZLINK_CONFIG_OK`(현재
state를 반복해도 포함). `NULL`이거나 유효하지 않은 handle이면 `ZLINK_CONFIG_INVALID_HANDLE`.
`zlink_receive_flow_state_t` 밖의 state면 `ZLINK_CONFIG_INVALID_ARGUMENT`. `DEALER`/`ROUTER`
이외의 socket 타입이면 `ZLINK_CONFIG_NOT_SUPPORTED`(completion lane이 없어 기존 byte HWM과
transport backpressure가 그대로 유지된다). 동시 진행 중인 close가 먼저 수락됐으면
`ZLINK_CONFIG_INVALID_STATE`.

**선택 기준.** `RUNNING`/`PAUSED`는 카운터가 아니라 절대 state다 — 현재 state를 반복해도
성공하며 새로 동기화되는 것은 없다. 완료는 socket을 소유한 runtime 스레드가 local state를
저장하는 시점이며, remote peer가 이미 그것을 관측했다는 뜻은 아니다.

---

전체 근거는 [Socket 공통 스펙](../spec/core/socket/README.ko.md)을 참고한다.
