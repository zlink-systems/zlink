
[레퍼런스 목차](README.ko.md)

# 03. Socket lifecycle

이 category는 모든 raw socket 타입에 공통인 진입점 — 생성, endpoint lifecycle
(bind/connect/disconnect), 종료, blocking 또는 completion 기반 send, 수신 flow control을 다룬다. Socket 타입별
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
`zlink_router_recv_part()`를 쓰고, `STREAM`만 첫 bind 또는 connect 전에 RAW part receive와
PACKET receive 중 하나를 고른다(STREAM category 참고). Socket은 context가 종료되기 전에
`zlink_close`로 닫아야 한다.

---

## `zlink_close`

Socket을 닫고 자원을 해제한다.

```c
zlink_close_result_t result = zlink_close(s);
```

**Parameters.** Socket handle만 받는다.

**Return과 errno.** `zlink_close_result_t`를 반환한다 — 성공하면 `ZLINK_CLOSE_OK`. Handle이
유효한 socket이 아니면 `ENOTSOCK`, 다른 스레드에서 이 handle에 operation이 진행 중이면
`EBUSY`.

**선택 기준.** 더 이상 필요 없는 socket을 해제할 때 호출한다. 남아 있는 send queue 메시지는
`ZLINK_OPT_LINGER`(Socket options and identity category)에 따라 버려지거나 전송된다. 공개
handle은 계층화된 스레드 안전성 계약을 따른다 — hot-path `send`는 동시 호출 가능하고,
control-path 호출(bind/connect/option/monitor)은 정합성을 위해 직렬화되며, `close`는 더
엄격한 fail-fast gate를 쓴다 — 일단 수락되면 같은 handle의 새 API 진입은 `ESHUTDOWN`으로
실패한다.

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

## `zlink_send_part` / `zlink_send_part_rid` / `zlink_completion_recv`

일반 part-send 함수는 `flags_`로 blocking local admission과 completion 기반 pending
admission을 고른다. `NONE FINAL`은 socket에서 스냅샷한 `ZLINK_OPT_SNDTIMEO`까지 기다리고,
`DONTWAIT FINAL`은 즉시 반환한다. Core가 record를 보관한 경우에만 0이 아닌 socket-local
completion ID를 반환한다.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_size);
memcpy(zlink_msg_data(&part), payload, payload_size);

zlink_completion_id_t id = 0;
zlink_submit_result_t r = zlink_send_part(
    s, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
    user_context, &id);

if (r == ZLINK_SUBMIT_OK && id != 0) {
    zlink_completion_t completion = {0};
    completion.struct_size = sizeof(completion);
    if (zlink_completion_recv(s, &completion, ZLINK_RECV_FLAGS_NONE)
        == ZLINK_RECV_OK) {
        /* 결과를 처리하기 전에 completion_id 또는 user_context를 대조한다. */
        zlink_completion_close(&completion);
    }
}
```

**Parameters.** `zlink_send_part()`는 Core가 logical target을 고르게 하고, routed socket은
logical RID와 `zlink_send_part_rid()`를 사용한다. Physical pair ID나 generation은 public
target이 아니다. 모든 part 호출은 성공과 실패 모두에서 `part_`를 소비한다. `MORE`는 part
하나를 staging하고 `FINAL`은 완성 record를 제출한다. 한 sequence는 같은 함수 family, target,
flag를 사용한다. `user_context_`는 DONTWAIT `FINAL`에서만 NULL이 아닐 수 있고 Core는 이를
되돌려 줄 뿐 읽거나 해제하지 않는다. 선택적인 ID output은 validation 전에 0으로 설정된다.

**Return과 errno.** 성공한 `NONE FINAL` 또는 즉시 admission된 `DONTWAIT FINAL`은 ID `0`으로
`ZLINK_SUBMIT_OK`를 반환하고 completion을 만들지 않는다. Core가 DONTWAIT record를 admission
전에 보관하면 0이 아닌 ID로 `ZLINK_SUBMIT_OK`를 반환하고 나중에 SEND completion 하나를 만든다.
Validation, target 부재, `SNDTIMEO`, pending limit 또는 completion slot 실패는 ID `0`으로
동기 반환되고 completion을 만들지 않는다. `zlink_completion_recv()`는 raw
`PAIR`/`DEALER`/`ROUTER`/`STREAM`에서만 지원한다. 성공적으로 받은 SEND 또는 REQUEST record는
모두 `zlink_completion_close()`로 닫는다.

**Pending ownership과 limit.** 성공한 DONTWAIT submit이 0이 아닌 ID를 반환한 뒤에는 Core가
완성 record와 admission 전 재시도를 소유한다. Application은 별도 retry queue를 만들거나 같은
payload를 다시 제출하지 않는다. Core는 transient reconnect 동안 같은 logical PAIR route,
설정된 DEALER endpoint 또는 routed RID에만 재시도한다. `ZLINK_OPT_PENDING_MAX_MSGS`와
`ZLINK_OPT_PENDING_MAX_BYTES`는 DONTWAIT SEND와 REQUEST가 공유하는 socket-local pool의 한도이며
`0`은 무제한이다. PAIR, DEALER, ROUTER, STREAM에서만 지원한다. 이전
0.16.0에서는 이전 send-scoped pending-limit 이름을 사용하지 않는다.

**Admission은 delivery가 아니다.** ID `0`과 `ZLINK_SEND_ADMITTED`는 local send queue
admission을 뜻한다. Peer receipt를 확인하거나 delivery ACK를 만들지 않으며 이후 disconnect에서
payload를 replay하지 않는다. 상관된 peer 응답이 필요하면 request/reply를 사용한다.

### Cancellation 경계

Core가 submit을 성공시킨 뒤에는 public operation-cancel API가 없다. Completion ID는 상관관계
값이지 제어 handle이 아니다. Core submit 전에는 application 또는 binding이 Core를 호출하지 않고
취소할 수 있고, Framework-owned queue도 submit 전에 work를 제거할 수 있다. Core가 record를
소유한 뒤 cancellation은 caller의 wait만 중단한다. Core는 늦게 admission할 수 있으며 socket
owner는 늦은 completion도 drain하고 close해야 한다. Socket close와 context termination은 별도
lifecycle cleanup 계약을 따른다.

---

## `zlink_socket_set_receive_flow_state`

이 socket의 local receive-flow state(`ZLINK_RECEIVE_FLOW_RUNNING` /
`ZLINK_RECEIVE_FLOW_PAUSED`)를 `DEALER` 또는 `ROUTER` socket에 설정한다. Transport lane은
socket 조합으로 정한다. DEALER-DEALER와 DEALER-ROUTER는 Application을, ROUTER-ROUTER는
Completion을 사용한다.

```c
zlink_config_result_t r =
  zlink_socket_set_receive_flow_state(s, ZLINK_RECEIVE_FLOW_PAUSED);
```

**Parameters.** `handle_`은 socket이다. `state_`는 `zlink_receive_flow_state_t`다
(`RUNNING = 0`, `PAUSED = 1`).

**Return과 errno.** `zlink_config_result_t`를 반환한다 — 성공하면 `ZLINK_CONFIG_OK`(현재
state를 반복해도 포함). `NULL`이거나 유효하지 않은 handle이면 `ZLINK_CONFIG_INVALID_HANDLE`.
`zlink_receive_flow_state_t` 밖의 state면 `ZLINK_CONFIG_INVALID_ARGUMENT`. `DEALER`/`ROUTER`
이외의 socket 타입이면 `ZLINK_CONFIG_NOT_SUPPORTED`이며, 지원하지 않는 타입은 기존 byte HWM과
transport backpressure를 그대로 유지한다. 동시 진행 중인 close가 먼저 수락됐으면
`ZLINK_CONFIG_INVALID_STATE`.

**선택 기준.** `RUNNING`/`PAUSED`는 카운터가 아니라 절대 state다 — 현재 state를 반복해도
성공하며 새로 동기화되는 것은 없다. 완료는 socket을 소유한 runtime 스레드가 local state를
저장하는 시점이며, remote peer가 이미 그것을 관측했다는 뜻은 아니다.

---

전체 근거는 [Socket 공통 스펙](../spec/core/socket/README.ko.md)을 참고한다.
