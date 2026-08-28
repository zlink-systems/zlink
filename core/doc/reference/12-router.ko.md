
[레퍼런스 목차](README.ko.md)

# 12. ROUTER

하나의 socket에서 여러 peer pipe를 관리하고 routing ID로 send target을 고르는 비동기 raw
socket이다. 일반 directed 메시지와 request/reply record를 routing ID로, 또는 정확히 하나의
transport pair에 고정해 처리한다. 정확한 signature는
[ROUTER 스펙](../spec/core/socket/07-router.ko.md)이 소유한다.

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

## `zlink_router_request_transport_pair_part`

정확히 하나의 transport pair로만 특정 peer에게 비동기 request를 제출한다.

```c
zlink_router_request_transport_pair_part(router, &peer_rid, target.transport_pair_id,
                                          target.transport_pair_generation, &part,
                                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                                          /*timeout_ms=*/3000, on_reply, userdata);
```

**Parameters.** `peer_rid_`, `transport_pair_id_`, `transport_pair_generation_`은 위
`zlink_send_part_transport_pair`(이 category)와 같은 exact pipe를 식별한다 — 나머지
parameter는 `zlink_router_request_part`의 중간·마지막 part 관례를 따른다.

**Return과 errno.** `zlink_submit_result_t`를 반환하며, `zlink_send_part_transport_pair`와
같은 exact-target 검증·재라우팅 없음·rollback 규칙을 따른다. Core는 request envelope가
wire에 보이기 전에 pending correlation을 등록한다 — 마지막 submit이 실패하면 그 pending
항목과 completion reservation을 제거하고 `handler_`를 호출하지 않는다.

**선택 기준.** `zlink_select_routed_submit_target`(이 category)으로 이미 고른 정확한
물리 연결로 추적되는 request를 보내야 할 때, Core가 그 routing ID의 현재 route를
재선택하게 두는 대신 `zlink_router_request_part` 대신 이걸 쓴다.

---

## `zlink_select_routed_submit_target`

Pipe credit을 점유하지 않고 나중의 exact-target submit을 위해 정확한 routed submit
target 하나를 snapshot한다.

```c
zlink_routed_submit_target_t target;
zlink_select_routed_submit_target(router, &peer_rid, &target);
```

**Parameters.** `router_rid_or_null_`은 ROUTER에서 필수이며 non-`NULL`이다 — snapshot할
정확한 admitted route를 가진 peer를 식별한다(DEALER에서는 같은 함수가 `NULL`을 요구하고
대신 하나의 weighted selection을 commit한다 — DEALER category 참고). `target_out_`은
`zlink_routed_submit_target_t`(peer routing ID, transport pair ID, transport pair
generation)를 받는다 — DONTWAIT exact submit 전에 binding이 소유한 pending state에
보관한다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 성공하면 `ZLINK_SUBMIT_OK`.
이 selection은 value snapshot이다 — pipe나 그 credit을 예약하지 않으므로, 그 사이 pipe가
detach되거나 generation이 바뀌면 `target_out_`을 쓴 나중의 exact submit이 여전히 실패할
수 있다.

**선택 기준.** `zlink_send_part_transport_pair`, `zlink_router_request_transport_pair_part`
(둘 다 이 category), 또는 DEALER의 exact-target send(DEALER category)를 submit하기 전에
binding이 필요로 하는 정확한 pipe identity를 얻을 때 쓴다. 애플리케이션이 이미 관측한
정확한 물리 연결에 multipart 시도를 고정해야 하고, Core가 그 routing ID의 현재 route를
재선택하게 두면 안 될 때 `zlink_send_part_rid`/`zlink_router_request_part` 대신 이걸
쓴다.

---

## `zlink_send_part_transport_pair`

정확히 하나의 transport pair로만 ROUTER raw part 하나를 보낸다 — 같은 routing ID를 쓰는
다른 연결로 재라우팅하지 않는다.

```c
zlink_send_part_transport_pair(router, &target_rid, target.transport_pair_id,
                                target.transport_pair_generation, &part,
                                ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `target_rid_`, `transport_pair_id_`, `transport_pair_generation_`은 같은
admitted peer를 식별해야 한다 — 보통 `zlink_select_routed_submit_target`이나 monitor
event의 pair identity에서 얻는다. `flags_`/`part_flag_`는 `zlink_send_part_rid`와 같은
관례를 따른다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 성공하면 `ZLINK_SUBMIT_OK`.
Exact pipe가 HWM이면 `ZLINK_SUBMIT_BACKPRESSURED`, detach되거나 generation이 stale이면
`ZLINK_SUBMIT_NOT_CONNECTED`다 — 둘 다 같은 routing ID의 다른 pipe로 재선택하지
않는다. 첫 part가 받아들여지면 exact-pipe fence가 `ZLINK_PART_FINAL`까지 유지되며,
실패하면 스테이징된 record 전체가 rollback된다.

**선택 기준.** 애플리케이션이 이미 선택한 정확한 물리 연결에 multipart send를 고정해야 할
때 `zlink_send_part_rid` 대신 이걸 쓴다 — 예를 들어 같은 routing ID로 peer가 재연결한
것을 관측하고 새 연결로 조용히 재라우팅되는 것을 피해야 할 때다.

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

## `zlink_router_recv_part_v2`

`zlink_router_recv_part`와 같은 계약으로 raw record part 하나를 수신하되, source transport
pair identity를 추가로 반환한다.

```c
const zlink_routing_id_t *source_rid;
uint64_t request_seq, pair_id, pair_generation;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_router_recv_part_v2(router, &source_rid, &request_seq, &pair_id, &pair_generation,
                           &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `zlink_router_recv_part`와 같은 필수 출력 pointer에 더해
`transport_pair_id_out_`, `transport_pair_generation_out_`가 있다.

**Return과 errno.** `zlink_router_recv_part`와 같다. 한 multipart record의 모든 part는
같은 routing ID, request sequence, pair ID, pair generation을 반환한다.

**선택 기준.** Caller가 record가 도착한 정확한 transport pair를 알아야 할 때 —
예를 들어 routing ID를 다시 해석해 다른 현재 pipe로 갈 위험 없이
`zlink_send_part_transport_pair`/`zlink_router_request_transport_pair_part`(이
category)로 reply하거나 후속 작업을 해야 할 때 — `zlink_router_recv_part` 대신 이걸
쓴다.

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

## 관련 범용 함수

`zlink_disconnect_transport_pair`(`socket/api.h`에서 `zlink_connect`/`zlink_disconnect`와
나란히 선언되어 있고, [socket README](../spec/core/socket/README.ko.md)가 규정)는 monitor
event의 pair id·generation으로 식별한 정확한 transport pair를 연결 해제한다 — 같은 peer
routing id를 쓰는 다른 연결에는 영향을 주지 않는다. ROUTER뿐 아니라 transport pair를
노출하는 모든 socket 타입에 적용되므로, 서술상 자리는 이 파일이 아니라 Socket lifecycle
category(`03-socket-lifecycle.ko.md`, `zlink_disconnect`/`zlink_disconnect_rid`와 나란히)다
— 위 `zlink_send_part_transport_pair`/`zlink_router_request_transport_pair_part`의 자연스러운
짝이라서 여기서만 언급해 둔다.

---

전체 근거는 [ROUTER 스펙](../spec/core/socket/07-router.ko.md)을 참고한다.
