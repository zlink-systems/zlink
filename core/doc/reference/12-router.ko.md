한국어 | [English](12-router.en.md)

[레퍼런스 목차](README.ko.md)

# 12. ROUTER

하나의 socket에서 여러 peer pipe를 관리하고 routing ID로 send target을 고르는 비동기 raw
socket이다. 일반 directed 메시지, request/reply record, 별도의 completion-control
채널을 처리한다. 정확한 signature는 [ROUTER 스펙](../spec/core/socket/07-router.ko.md)이
소유한다.

---

## `zlink_set_router_option` / `zlink_get_router_option`

ROUTER 전용 옵션을 설정하거나 읽는다.

```c
int mandatory = 1;
zlink_set_router_option(router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof(mandatory));
```

**Parameters.** `option_`은 `ZLINK_ROUTER_OPT_MANDATORY`(`int` 0/1, 기본 `1` — `1`이면
연결된 pipe가 없는 routing ID로의 directed submit이
`ZLINK_SUBMIT_NOT_CONNECTED`로 실패), `ZLINK_ROUTER_OPT_PROBE`(`int` 0/1, 기본 `0` —
DEALER의 probe 옵션과 같은 의미), `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID`(가변 길이
byte string, 설정 전용 — *다음* `zlink_connect`가 만들 pipe의 local alias를 설정한다 —
매 connect 전에 설정한다), `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS`(음이 아닌 `int` ms,
기본 `5000`), 또는 `ZLINK_ROUTER_OPT_WEIGHT`(`int` `0..10000`, 기본 `100`)다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. ROUTER 전용이 아닌 HWM·reconnect·timeout 옵션은
`zlink_set_option`/`zlink_get_option`(Socket options and identity category)을 쓴다.

**선택 기준.** 특정 outbound pipe에 대해 socket 자체의 routing identity
(`zlink_set_routing_id`, Socket options and identity category)와 별개로 예측 가능한
local alias가 필요하면 `zlink_connect` 직전에 `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID`를
설정한다.

---

## `zlink_send_part_rid`

Routing ID로 지정한 특정 peer에게 일반 directed raw multipart part 하나를 보낸다 —
request sequence나 reply를 기대하지 않는다.

```c
zlink_send_part_rid(router, &target_rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `target_rid_`가 목적지 peer를 식별한다 — 같은 multipart message의 모든
part가 같은 target을 쓴다. `flags_`는 `ZLINK_SEND_FLAGS_NONE` 또는
`ZLINK_SEND_FLAGS_DONTWAIT`다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 성공하면
`ZLINK_SUBMIT_OK`. `ZLINK_ROUTER_OPT_MANDATORY`가 `1`이고 `target_rid_`에 연결된
pipe가 없으면 `ZLINK_SUBMIT_NOT_CONNECTED`.

**선택 기준.** Reply 추적이 필요 없는 one-way directed 메시지에 쓴다. ROUTER의 다른
`*_part` send family와 마찬가지로 `ZLINK_PART_MORE`부터 `ZLINK_PART_FINAL`까지의
sequence는 같은 handle에서 다른 send-helper family나 다른 routing ID를 끼워 넣지 않고
완결해야 한다 — 실패하면 그 record에 스테이징된 모든 part가 원자적으로 폐기된다.

---

## `zlink_router_request_part`

특정 peer에게 비동기 request payload를 part 단위로 제출하며, callback을 통한 reply를
기대한다.

```c
zlink_router_request_part(router, &peer_rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                           /*timeout_ms=*/3000, on_reply, userdata);
```

**Parameters.** `peer_rid_`가 request target이다. 중간·마지막 part 관례는 DEALER의
`zlink_dealer_request_part`(DEALER category)와 같다 — 중간 part는
`ZLINK_PART_MORE`/`timeout_ms_ == 0`/`handler_ == NULL`/`userdata_ == NULL`을 쓰고,
마지막 part는 `ZLINK_PART_FINAL`과 non-`NULL` `handler_`를 쓰며 `timeout_ms_ == 0`이면
`ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` 기본값을 쓴다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 마지막 part에서
`ZLINK_SUBMIT_OK`는 나중에 정확히 하나의 완료가 `handler_`에 도달함을 뜻한다. 실패한
submit은 절대 호출하지 않는다. Callback의 `parts_` 소유권은 callback으로 이전된다.

**선택 기준.** Directed 메시지에 특정 peer로부터의 추적되는 reply가 필요할 때
`zlink_send_part_rid` 대신 이걸 쓴다.

---

## `zlink_router_recv_part`

완결된 raw record의 part 하나 — 일반 directed 메시지나 수신된 request — 를 수신한다.

```c
const zlink_routing_id_t *source_rid;
uint64_t request_seq;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_router_recv_part(router, &source_rid, &request_seq, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** 모든 출력 pointer가 필수다. `source_node_rid_out_`은 Core가 소유한
thread-local view다 — 이 스레드의 다음 raw receive 호출 이후에도 살려야 하면 복사한다 —
다음 호출이 이전 view를 무효화한다.

**Return과 errno.** `zlink_recv_result_t`를 반환한다 — 성공하면 `ZLINK_RECV_OK`이며
수신한 part 소유권이 caller에게 이전된다. Non-blocking 호출인데 받을 게 없으면
`ZLINK_RECV_NO_DATA`와 `EAGAIN`.

**선택 기준.** 출력 조합으로 record 종류를 구분한다 — `request_seq_out_ == 0`이면 일반
raw multipart 메시지다. 0이 아니면 수신된 request이며 그 sequence가
`zlink_router_reply_part`에 넘길 reply token이고, 같은 `source_node_rid_out_`과
짝지어진다. `zlink_router_request_part`로 시작한 작업의 reply와 종료 실패는 여기 나타나지
않고 그 호출의 callback으로 전달된다. `has_more_out_ == ZLINK_PART_MORE`면 다음 호출이
같은 record를 이어받는다.

---

## `zlink_router_reply_part`

이 ROUTER가 받은 request record에 대한 reply part를 보낸다.

```c
zlink_router_reply_part(router, &peer_rid, request_seq, &reply_part, ZLINK_PART_FINAL);
```

**Parameters.** `peer_rid_`와 `request_seq_`는 그 request record에 대해
`zlink_router_recv_part`가 반환한 값과 정확히 같아야 한다. Multipart reply는 모든
part에 둘 다 재사용한다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 마지막 part가 성공하면 reply가
완결된다. Reply-sequence 실패 시, token/peer-RID 쌍은 성공적인 마지막 part 또는
request-lifecycle 종료 중 먼저 오는 것까지 유효하므로 보관해 둔 완결된 reply를 첫 part부터
재제출할 수 있다.

**선택 기준.** `zlink_router_recv_part`가 0이 아닌 sequence로 드러낸 request에 답할 때
쓴다.

---

## `zlink_router_completion_control_handler` / `zlink_router_completion_control_part`

Completion connection에서 유계 control record를 처리할 handler를 등록하고 실제로
보낸다 — 일반 directed 메시지·request는 Application connection에 남아 있는 것과
분리되어 있다.

```c
zlink_router_completion_control_handler(router, on_control, userdata);
// ...
zlink_router_completion_control_part(router, &peer_rid, &control_part, ZLINK_PART_FINAL);
```

**Parameters.** `handler_`는 `zlink_completion_control_handler_fn`이다 — socket마다
handler가 하나이며, 다시 등록하면 교체된다. Core는 record 내용을 해석하지 않는다 — payload에
대한 command 종류, allowlist, application 의미를 정의하지 않는다.

**Return과 errno.** Handler 등록은 `zlink_handler_result_t`를 반환한다 — `NULL`
handler면 `ZLINK_HANDLER_INVALID_ARGUMENT`, non-ROUTER socket이면
`ZLINK_HANDLER_NOT_SUPPORTED`. 전송은 `zlink_submit_result_t`를 반환하며 다른 send
family(DEALER category)와 같은 part-sequencing·매 결과마다 소비 규칙을 따른다 —
Completion connection은 유한한 byte HWM을 가지므로 `ZLINK_SUBMIT_BACKPRESSURED` 결과는
보관해 둔 복사본으로 send-ready 뒤 record 전체를 첫 part부터 재시도하라는 뜻이다. 등록된
handler가 없는 record는 버려진다.

**선택 기준.** 같은 연결의 application 메시지·request 트래픽과 경합하면 안 되는
인프라 수준 신호에만 쓴다 — 새 socket이나 connection을 만들지 않는다. Handler는
completion owner가 connection을 처리할 때 실행되므로, application receive 호출 없이도
`ZLINK_POLLCOMPLETION` poller(Polling and pollers category)가 control을 받을 수
있다. Callback 안의 `source_rid_`는 callback이 반환될 때까지만 유효하다. Callback이
실행 중일 때 socket을 닫으면 `ZLINK_CLOSE_BUSY`/`EBUSY`로 실패한다 — callback이
반환된 뒤 재시도한다.

---

전체 근거는 [ROUTER 스펙](../spec/core/socket/07-router.ko.md)을 참고한다.
