한국어 | [English](11-dealer.en.md)

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

## `zlink_dealer_recv_part`

DEALER socket에서 완결된 record의 part 하나를 종류와 함께 수신한다.

```c
uint8_t message_type;
uint64_t request_seq;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_dealer_recv_part(dealer, &message_type, &request_seq, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** 모든 출력 pointer가 필수다 — `message_type_out_`(`zlink_dealer_message_type_t`
값을 담는 `uint8_t`), `request_seq_out_`, `part_out_`(이미 초기화됨), `has_more_out_`.
`flags_`는 `ZLINK_RECV_FLAGS_NONE` 또는 `ZLINK_RECV_FLAGS_DONTWAIT`다.

**Return과 errno.** `zlink_recv_result_t`를 반환한다 — 성공하면 `ZLINK_RECV_OK`이며
수신한 part 소유권이 caller에게 이전된다(Message category). Non-blocking 호출인데 받을
게 없으면 `ZLINK_RECV_NO_DATA`와 `EAGAIN`.

**선택 기준.** `message_type_out_`으로 record를 분류한다 — `ZLINK_DEALER_MESSAGE_RAW`(`0`,
`request_seq_out_ == 0`)는 request/reply envelope이 없는 일반 raw multipart
메시지다. `ZLINK_DEALER_MESSAGE_REQUEST`(`1`, 0이 아닌 sequence)는 이 DEALER가 받은
request다 — sequence가 `zlink_dealer_reply_part`에 넘길 reply token이다.
`ZLINK_DEALER_MESSAGE_REPLY`(`2`)와 `ZLINK_DEALER_MESSAGE_ERROR_REPLY`(`3`)는 reply
record다 — 실제로는 `zlink_dealer_request_part`로 시작한 작업의 reply와 종료 실패는
이 수신 호출이 아니라 그 호출의 `zlink_reply_handler_fn` 완료로 전달된다. 한 multipart
record의 모든 part는 같은 type과 sequence를 반환한다 — `has_more_out_ ==
ZLINK_PART_MORE`면 다음 호출이 같은 record를 이어받는다.

---

## `zlink_dealer_request_part`

Callback을 통한 reply를 기대하며 비동기 request payload를 part 단위로 제출한다.

```c
zlink_dealer_request_part(dealer, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                           /*timeout_ms=*/3000, on_reply, userdata);
```

**Parameters.** 중간 part는 `ZLINK_PART_MORE`, `timeout_ms_ == 0`, `handler_ == NULL`,
`userdata_ == NULL`을 넘긴다. 마지막 part는 `ZLINK_PART_FINAL`과 non-`NULL`
`handler_`(`zlink_reply_handler_fn`)를 쓴다 — 그 마지막 호출에서 `timeout_ms_ == 0`이면
타임아웃 없음이 아니라 `ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS` 기본값을 쓴다. `flags_`는
`ZLINK_SEND_FLAGS_NONE` 또는 `ZLINK_SEND_FLAGS_DONTWAIT`다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 마지막 part에서
`ZLINK_SUBMIT_OK`는 나중에 정확히 하나의 완료가 `handler_`에 도달함을 뜻한다. 실패한
submit은 handler를 절대 호출하지 않는다. Callback의 `parts_`와 모든 메시지의 소유권은
callback으로 이전되며 callback이 정확히 한 번 해제한다. `zlink_request_result_t`(여기서
반환되는 게 아니라 callback으로 전달됨)가 timeout과 다른 종료 결과를 식별한다.

**선택 기준.** 메시지에 추적되는 reply가 필요할 때 `zlink_send_part`(PAIR category) 대신
이걸 쓴다. 다른 `*_part` send family와 마찬가지로 `ZLINK_PART_MORE`부터 `ZLINK_PART_FINAL`
까지의 sequence는 같은 handle에서 다른 send-helper family를 끼워 넣지 않고 완결해야
한다 — 어느 시점에서든 실패하면 그 record에 스테이징된 모든 part가 원자적으로 폐기되고(peer
에게는 아무것도 보이지 않음), 실패한 호출의 `part_`은 여전히 소비되며, request sequence나
handler 호출은 생기지 않는다 — 보관해 둔 복사본으로 record 전체를 처음 part부터 재시도한다.

---

## `zlink_dealer_reply_part`

이 DEALER가 받은 request record에 대한 reply part를 보낸다.

```c
zlink_dealer_reply_part(dealer, request_seq, &reply_part, ZLINK_PART_FINAL);
```

**Parameters.** `request_seq_`는 같은 socket에서 그 request에 대해
`zlink_dealer_recv_part`가 반환한 0이 아닌 reply token이어야 한다. `part_`/`part_flag_`는
다른 send family와 같은 multipart 규칙을 따른다 — multipart reply는 모든 part에 같은
token을 재사용한다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 마지막 part가 성공하면
`ZLINK_SUBMIT_OK`이며 그 token에 대한 reply가 완결되어 재사용할 수 없게 된다.
Reply-sequence 실패 시, token은 성공적인 `ZLINK_PART_FINAL` 또는 request-lifecycle
종료 중 먼저 오는 것까지 유효하므로 보관해 둔 완결된 reply를 첫 part부터 재제출할 수 있다.

**선택 기준.** `zlink_dealer_recv_part`가 `ZLINK_DEALER_MESSAGE_REQUEST`로 분류한
record에 답할 때 쓴다. `ZLINK_POLLIN`(Polling and pollers category)은 raw나 request/reply
record를 받을 수 있다는 뜻이고, `ZLINK_POLLOUT`/`zlink_send_ready_handler`(Socket
lifecycle category)는 backpressure된 submit을 재시도할 가치가 있다는 뜻이지 성공을
보장하지 않는다.

---

전체 근거는 [DEALER 스펙](../spec/core/socket/06-dealer.ko.md)을 참고한다. Raw one-way
send는 `zlink_send_part`(PAIR category)를 쓴다 — 여기서 반복하지 않는다.
