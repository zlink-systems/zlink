
[레퍼런스 목차](README.ko.md)

# 12. ROUTER

하나의 socket에서 여러 peer pipe를 관리하고 routing ID로 send target을 고르는 비동기 raw
socket이다. 일반 directed 메시지와 request/reply record를 logical routing ID로 처리한다.
정확한 signature는
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
reply token을 만들지 않고 reply를 기다리지 않는다.

```c
zlink_send_part_rid(router, &target_rid, &part, ZLINK_SEND_FLAGS_NONE,
                    ZLINK_PART_FINAL, NULL, NULL);
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

## `zlink_request_part`(ROUTER)

특정 logical ROUTER peer에 request payload part 하나를 제출한다. Reply 또는 terminal 결과는
socket-local completion queue에서 pull한다.

```c
zlink_completion_id_t id = 0;
zlink_request_part(router, &peer_rid, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                   ZLINK_PART_FINAL, 3000, user_context, &id);
```

**Parameters.** `target_router_rid_or_null_`은 ROUTER peer의 non-NULL logical RID다. MORE는
timeout `0`과 NULL context가 필요하다. FINAL은 명시적인 reply timeout을 받고 `0`은
`ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS`를 고른다. 성공한 FINAL은 항상 0이 아닌 ID를 반환한다.

**Return과 errno.** 성공한 FINAL은 REQUEST completion 하나를 예약한다. Admission 전 실패는
ID `0`으로 반환되고 completion을 만들지 않는다. Completion은 `request_result`와, OK이면
reply multipart를 담으며 `zlink_completion_close()`로 해제한다.

**선택 기준.** Directed 메시지에 특정 peer로부터의 추적되는 reply가 필요할 때
`zlink_send_part_rid` 대신 이걸 쓴다.

---

## Request completion receive

REQUEST reply, timeout과 terminal 결과는 `zlink_router_recv_part()`의 DATA로 나타나지 않는다.

```c
zlink_completion_t completion = {0};
completion.struct_size = sizeof(completion);
zlink_completion_recv(router, &completion, ZLINK_RECV_FLAGS_NONE);
/* completion.completion_id 또는 completion.user_context를 대조한다. */
zlink_completion_close(&completion);
```

**Parameters.** Output은 caller-owned이고 정확한 `struct_size`를 가지며 나머지는 비어 있어야
한다. `flags_`는 NONE 또는 DONTWAIT다. REQUEST OK는 close 전까지 연속 reply 배열을 소유한다.

**Return과 errno.** 빈 queue의 DONTWAIT receive는 `ZLINK_RECV_NO_DATA`와 `EAGAIN`이다.
Payload 없는 terminal record를 포함해 성공적으로 받은 record를 모두 닫는다.

**선택 기준.** `ZLINK_POLLCOMPLETION` 뒤 NO_DATA까지 drain한다. Completion ID 또는 context로
상관시키며 completion 순서는 submit 순서나 target별 wire 순서와 다를 수 있다.

---

## Logical routed target

Public send target은 physical connection identifier나 generation이 아니라 logical routing
ID다. Core는 FINAL을 admit하면서 현재 route를 해석한다.

```c
zlink_send_part_rid(router, &peer_rid, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                    ZLINK_PART_FINAL, user_context, &completion_id);
```

**Parameters.** `peer_rid`는 receive metadata에서 복사했거나 application이 따로 설정한
application-visible identifier다. Public physical-pair field를 포함하지 않는다.

**Return과 errno.** Logical RID가 없으면 ROUTER mandatory 계약을 따른다. DONTWAIT admission이
pending이면 성공한 submit은 선택된 pipe를 노출하거나 고정하는 대신 0이 아닌 completion ID를
반환한다.

**선택 기준.** Application routing에 안정적인 identity가 필요하면 logical RID를 보관한다.
Monitor connection ID를 send capability로 cache하지 않는다. Reconnect가 physical connection을
교체해도 logical target은 유지될 수 있다.

---

## Pending same-RID admission

Local send queue에 들어갈 수 없는 DONTWAIT FINAL은 logical target에 대해 Core가 보관할 수 있다.

```c
zlink_completion_id_t id = 0;
zlink_submit_result_t result = zlink_send_part_rid(
    router, &target_rid, &part, ZLINK_SEND_FLAGS_DONTWAIT,
    ZLINK_PART_FINAL, user_context, &id);
```

**Parameters.** 완전한 multipart에서 target RID와 flags는 고정된다. Core는 성공과 실패 모두에서
모든 part를 소비한다. Application이 동기 실패에서 복구해야 하면 submit 전에 완성 record의 별도
복사본을 보관한다.

**Return과 errno.** ID `0`은 즉시 local admission되어 completion이 없다는 뜻이다. 0이 아닌
ID는 Core가 pending record를 소유하고 이후 SEND completion 하나를 만든다는 뜻이다. Pending
pool 또는 completion slot 고갈은 ID `0`으로 동기 거부되고 completion을 만들지 않는다.

**선택 기준.** 접수된 pending record를 caller retry queue에 넣지 않는다. Core는 transient
reconnect 동안 같은 logical RID를 기다린다. Admission 뒤에는 payload를 replay하지 않으며
`ZLINK_SEND_ADMITTED`를 peer-delivery ACK로 바꾸지도 않는다.

---

## `zlink_router_recv_part`

완결된 raw record의 part 하나 — 일반 directed 메시지나 수신된 request — 를 수신한다.

```c
const zlink_routing_id_t *source_rid;
zlink_reply_token_t reply_token;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_router_recv_part(router, &source_rid, &reply_token, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** 모든 출력 pointer가 필수다. `source_rid_out_`은 socket-owned view다. 같은
socket의 다음 data receive 진입 뒤에도 필요하면 복사한다. 그 진입은 성공 여부와 관계없이 이전
view를 무효화한다. `reply_token_out_`은 불투명하다.

**Return과 errno.** `zlink_recv_result_t`를 반환한다 — 성공하면 `ZLINK_RECV_OK`이며
수신한 part 소유권이 caller에게 이전된다. Non-blocking 호출인데 받을 게 없으면
`ZLINK_RECV_NO_DATA`와 `EAGAIN`.

**선택 기준.** Token으로 record 종류를 구분한다. Token `0`은 일반 DATA이고 0이 아니면
수신된 REQUEST다. REQUEST는 같은 RID와 token을 `zlink_reply_part()`에 넘긴다.
`zlink_request_part()`로 시작한 작업의 reply와 terminal 실패는 여기 나타나지 않고 completion
queue에 들어간다. `has_more_out_ == ZLINK_PART_MORE`면 다음 호출이
같은 record를 이어받는다.

---

## 수신 RID와 token 수명

ROUTER multipart의 모든 part는 같은 logical RID와 reply token을 반환한다. 어느 값도 physical
transport-pair identity를 노출하지 않는다.

```c
/* 이 router에서 다음 data receive 전에 RID를 복사한다. */
zlink_routing_id_t rid_copy = *source_rid;
zlink_reply_token_t token_copy = reply_token;
```

**Parameters.** RID byte는 borrowed socket-owned view다. Token은 source RID와 이를 만든
ROUTER에 연결된 scalar opaque capability다.

**Return과 errno.** Completion, monitor, poller 호출은 RID view를 무효화하지 않는다. 같은
socket의 다음 DATA receive 진입이 무효화한다. 다른 socket의 receive도 영향을 주지 않는다.

**선택 기준.** 다른 receive가 먼저 실행될 수 있으면 RID를 즉시 복사한다. Token은 그 REQUEST에
reply할 때만 그대로 보존하고 크기를 비교하거나 wire sequence를 추론하지 않는다.

---

## `zlink_reply_part`

이 ROUTER가 받은 request record에 대한 reply part를 보낸다.

```c
zlink_reply_part(router, &peer_rid, reply_token, &reply_part, ZLINK_PART_FINAL);
```

**Parameters.** `source_rid_`와 `reply_token_`은 그 REQUEST record에 대해
`zlink_router_recv_part()`가 반환한 값과 정확히 같아야 한다. Multipart reply는 모든
part에 둘 다 재사용한다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 마지막 part가 성공하면 reply가
완결된다. Reply-sequence 실패 시, token/peer-RID 쌍은 성공적인 마지막 part 또는
request-lifecycle 종료 중 먼저 오는 것까지 유효하므로 보관해 둔 완결된 reply를 첫 part부터
재제출할 수 있다.

**선택 기준.** `zlink_router_recv_part()`가 0이 아닌 token으로 드러낸 REQUEST에 답할 때
쓴다. DEALER-ROUTER reply는 Application lane을 사용해 DATA 뒤에서 backpressure될 수 있고,
ROUTER-ROUTER reply는 Completion lane을 사용한다.

---

## 관련 범용 함수

`zlink_disconnect_rid()`(`socket/api.h`에서 `zlink_connect`/`zlink_disconnect`와 나란히
선언되고 [socket README](../spec/core/socket/README.ko.md)가 규정)는 logical RID로 선택한 현재
peer connection의 종료를 요청한다. 성공은 요청이 접수됐다는 뜻이지 remote shutdown 완료가
아니다. 중복 RID로 lookup이 모호하면 physical pair selector를 노출하지 않고 conflict를 보고한다.

---

전체 근거는 [ROUTER 스펙](../spec/core/socket/07-router.ko.md)을 참고한다.
