
[레퍼런스 목차](README.ko.md)

# 11. DEALER

수신 메시지를 공평하게 큐잉하고 round-robin/weight로 outbound peer를 고르는 비동기 raw
socket이다. 같은 socket이 일반 raw 메시지와 request/reply record를 함께 처리한다. DEALER는
raw send 쪽에서 `zlink_send_part`를 PAIR(PAIR category)와 변경 없이 공유한다 — 이
category는 DEALER 고유의 옵션과 request/reply data plane을 다룬다. 정확한 signature는
[DEALER 스펙](../spec/core/socket/06-dealer.ko.md)이 소유한다.

---

## `zlink_set_dealer_option` / `zlink_get_dealer_option`

DEALER 전용 옵션을 설정하거나 읽는다.

```c
int weight = 300;
zlink_set_dealer_option(dealer, ZLINK_DEALER_OPT_WEIGHT, &weight, sizeof(weight));
```

**Parameters.** `option_`은 `ZLINK_DEALER_OPT_PROBE`(`int` 0/1, 기본 `0` — 연결 시 빈 raw
메시지를 보내 peer가 연결과 routing ID를 관측할 수 있게 함),
`ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS`(음이 아닌 `int` ms, 기본 `5000` —
`timeout_ms_ == 0`일 때 request 호출이 쓰는 기본값), 또는 `ZLINK_DEALER_OPT_WEIGHT`(`int`
`0..10000`, 기본 `100` — 이 DEALER가 광고하는 weight — 범위 밖 값은 거부되며 clamp되지
않음)다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. DEALER 전용이 아닌 HWM·reconnect·timeout 옵션은
`zlink_set_option`/`zlink_get_option`(Socket options and identity category)을 쓴다.

**선택 기준.** `ZLINK_DEALER_OPT_WEIGHT`를 조정해 outbound peer 선택을 편향시킨다 —
candidate는 광고된 weight가 양수인 연결된 peer다(weight `0`은 candidate 집합에서
제외된다 — 알려진 peer 전부가 weight `0`이면 submit이 `ZLINK_SUBMIT_NOT_ADMITTED`로
실패할 수 있다). 선택은 candidate마다 running value를 쓰는 알고리즘으로 진행된다(weight를
더하고, 가장 큰 running value를 가진 candidate를 고르며 — 동점이면 가장 작은 routing-ID
식별자로 결정 — 승자의 running value에서 candidate 집합의 총 weight를 뺀다). 이는 각
candidate의 몫을 한 번에 몰아주지 않고 설정된 비율대로 연속 전송을 분산시킨다 — 식별자가
다르기만 하면 같은 peer와 weight를 쓰는 두 프로세스는 같은 순서를 만든다. 지금 당장 write를
받을 수 없는(backpressure) candidate는 그 메시지에서만 건너뛰되 running value는 잃지
않으며, 다시 용량을 알리면 복귀한다. 크기 제한을 넘어 거부된 메시지는 다른 candidate로
재시도하지 않는다.

---

## `zlink_send_part`(DEALER)

일반 DATA part 하나를 제출한다. Core는 FINAL record를 제출할 때 호환되는 양의 weight
logical route 하나를 고른다.

```c
zlink_completion_id_t id = 0;
zlink_send_part(dealer, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                ZLINK_PART_FINAL, user_context, &id);
```

**Parameters.** Multipart는 MORE부터 FINAL까지 같은 함수와 flags를 사용한다.
`user_context_`는 DONTWAIT FINAL에서만 NULL이 아닐 수 있다. 선택적인 completion ID는 MORE,
blocking NONE, 즉시 DONTWAIT admission에서 0이다.

**Return과 errno.** `NONE FINAL`은 `SNDTIMEO` 안에서 local admission을 기다린다. DONTWAIT는
즉시 반환한다. Core가 admission 전에 record를 보관하면 0이 아닌 ID와 이후 SEND completion
하나를 반환한다. 실패한 part는 입력을 소비하고 staged record 전체를 rollback한다.

**선택 기준.** One-way DATA에 쓴다. Core가 DONTWAIT record를 보관한 뒤에는 FINAL에서 고른
configured endpoint 재시도를 Core가 소유한다. Application이 같은 payload를 queue에 넣거나
다시 제출하지 않는다. Admission completion은 peer delivery 확인이 아니다.

---

## `zlink_recv_part`(DEALER)

DEALER socket에서 일반 DATA record의 part 하나를 받는다.

```c
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_recv_part(dealer, NULL, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `source_rid_out_`은 선택 사항이며 DEALER에서는 `NULL`이다. `part_out_`은
초기화된 message여야 하고 `has_more_out_`은 MORE 또는 FINAL을 받는다. `flags_`는
`ZLINK_RECV_FLAGS_NONE` 또는 `ZLINK_RECV_FLAGS_DONTWAIT`다.

**Return과 errno.** `zlink_recv_result_t`를 반환한다 — 성공하면 `ZLINK_RECV_OK`이며
수신한 part 소유권이 caller에게 이전된다(Message category). Non-blocking 호출인데 받을
게 없으면 `ZLINK_RECV_NO_DATA`와 `EAGAIN`.

**선택 기준.** DEALER는 여기서 일반 DATA만 받는다. 제출한 request의 reply, timeout과 terminal
결과는 DATA가 아니라 `zlink_completion_recv()`의 REQUEST record로만 나타난다.
`has_more_out_ == ZLINK_PART_MORE`이면 다음 호출이 같은 DATA record를 이어받는다.

---

## `zlink_request_part`(DEALER)

Request payload를 part 단위로 제출한다. Reply 또는 terminal 결과는 socket-local completion
queue에서 pull한다.

```c
zlink_completion_id_t id = 0;
zlink_request_part(dealer, NULL, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                   ZLINK_PART_FINAL, 3000, user_context, &id);
```

**Parameters.** DEALER는 Core가 ready ROUTER logical route 하나를 고르므로 target에 `NULL`을
넘긴다. MORE는 timeout `0`과 NULL context가 필요하다. FINAL은 명시적인 reply timeout을
받고 `0`은 `ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS`를 고른다. 성공한 FINAL은 항상 0이 아닌
ID를 반환한다.

**Return과 errno.** 성공한 FINAL은 correlation과 REQUEST completion 하나를 예약한다.
Admission 전 실패는 ID `0`으로 반환되고 completion을 만들지 않는다. 성공 뒤
`zlink_completion_recv()`가 `request_result`와, OK이면 reply multipart를 반환한다. 모든
record를 `zlink_completion_close()`로 닫는다.

**선택 기준.** 메시지에 추적되는 reply가 필요할 때 `zlink_send_part`(PAIR category) 대신
이걸 쓴다. 다른 `*_part` send family와 마찬가지로 `ZLINK_PART_MORE`부터 `ZLINK_PART_FINAL`
까지의 sequence는 같은 handle에서 다른 send-helper family를 끼워 넣지 않고 완결해야
한다 — 어느 시점에서든 실패하면 그 record에 스테이징된 모든 part가 원자적으로 폐기되고(peer
에게는 아무것도 보이지 않음), 실패한 호출의 `part_`은 여전히 소비되며, request correlation이나
completion은 생기지 않는다 — 보관해 둔 복사본으로 record 전체를 처음 part부터 재시도한다.

---

## `zlink_completion_recv`(DEALER)

DEALER completion queue에서 pending-send 또는 request terminal record 하나를 pull한다.

```c
zlink_completion_t completion = {0};
completion.struct_size = sizeof(completion);
zlink_recv_result_t rc = zlink_completion_recv(
    dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
if (rc == ZLINK_RECV_OK)
    zlink_completion_close(&completion);
```

**Parameters.** Output은 정확한 `struct_size`를 갖고 나머지는 비어 있어야 한다. `flags_`는
NONE 또는 DONTWAIT다. `completion_id`나 `user_context`로 operation을 찾는다. Resolver 순서는
submit 순서가 아니다.

**Return과 errno.** 빈 queue에 DONTWAIT를 쓰면 `ZLINK_RECV_NO_DATA`와 `EAGAIN`이다.
성공한 REQUEST record는 `zlink_completion_close()` 전까지 reply 배열을 소유한다.

**선택 기준.** `ZLINK_POLLCOMPLETION` 뒤에는 NO_DATA까지 반복해서 drain한다. 이 queue는
DATA receive와 분리된다. DEALER-ROUTER REPLY가 앞선 DATA와 physical Application FIFO를
공유하므로 그 DATA 뒤에서 늦어질 수 있다는 점은 별도다.

---

## DEALER responder 경계

DEALER는 request originator이지 typed REQUEST responder가 아니다. DEALER reply-part 함수가
없으며 `zlink_recv_part()`는 reply token을 반환하지 않는다.

```c
/* Responder는 ROUTER와 zlink_router_recv_part()의 token을 사용한다. */
zlink_reply_part(router, source_rid, reply_token, &reply_part, ZLINK_PART_FINAL);
```

**Parameters.** `reply_token`은 ROUTER request receive만 반환하는 socket-owned 불투명
capability다. Wire sequence가 아니며 합성하거나 해석하지 않는다.

**Return과 errno.** `zlink_reply_part()`는 source RID, token과 owner ROUTER를 검증한다.
성공한 FINAL만 token을 소비하며 실패하면 request lifecycle이 유지되는 동안 완전한 reply를
재시도할 수 있다.

**선택 기준.** DEALER request submit은 ROUTER responder와 짝지어 쓴다. DEALER의
`ZLINK_POLLIN`은 DATA를 받을 수 있다는 뜻이고 `ZLINK_POLLCOMPLETION`은 SEND 또는 REQUEST
completion을 drain할 수 있다는 뜻이다. 어느 readiness도 peer delivery나 다음 submit 성공을
보장하지 않는다.

---

전체 근거는 [DEALER 스펙](../spec/core/socket/06-dealer.ko.md)을 참고한다. Raw one-way
send는 `zlink_send_part`(PAIR category)를 쓴다 — 여기서 반복하지 않는다.
