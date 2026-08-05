한국어 | [English](08-sub.en.md)

[레퍼런스 목차](README.ko.md)

# 08. SUB

Topic 필터링을 하는 subscribe raw socket 타입이다. SUB은 데이터에 대해 수신 전용이다 —
구독 관리가 제어 평면이다. `zlink_set_subscription`/`zlink_unset_subscription`/
`zlink_subscription_at`를 XSUB와 공유한다(XSUB category는 중복하지 않고 여기를
교차 참조한다). 정확한 signature는 [SUB 스펙](../spec/core/socket/03-sub.ko.md)이
소유한다.

---

## `zlink_set_sub_option` / `zlink_get_sub_option`

SUB/XSUB 전용 옵션을 설정하거나 읽는다.

```c
int topics;
size_t len = sizeof(topics);
zlink_get_sub_option(s, ZLINK_SUB_OPT_TOPICS_COUNT, &topics, &len);
```

**Parameters.** `option_`은 `ZLINK_SUB_OPT_TOPICS_COUNT`다 — SUB 전용 옵션은 이것 하나뿐이며
읽기 전용이다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. 모든 socket 타입이 공유하는 옵션은
`zlink_set_option`/`zlink_get_option`(Socket options and identity category)을 쓴다.

**선택 기준.** Socket에 현재 등록된 topic 필터 개수를 확인할 때 쓴다.

---

## `zlink_set_subscription` / `zlink_unset_subscription`

Raw SUB나 XSUB socket에 byte-prefix topic 필터를 추가하거나 제거한다.

```c
zlink_set_subscription(s, "game.scores.");
// ... 나중에 ...
zlink_unset_subscription(s, "game.scores.");
```

**Parameters.** `filter_`는 내부에 NUL이 없는 널 종료 문자열이다. Terminator 앞의 바이트가
byte-prefix 필터다 — 메시지의 topic이 그 바이트로 시작하면 매치된다. 빈 문자열은 전체
구독이다. Wildcard 문법은 없다 — 끝의 `*`는 문자 그대로 매치된다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. `handle_`이 `NULL`이면 `EFAULT`. `filter_`가 `NULL`이거나 handle
타입이 subscribe/unsubscribe를 지원하지 않으면 `EINVAL`.

**선택 기준.** Raw SUB와 raw XSUB에 적용된다. `unset_subscription`의 filter는 이전에 등록된
prefix와 정확히 일치해야 한다(`set_subscription`과 같은 byte-prefix 해석).

---

## `zlink_subscribe_part`

Raw SUB나 XSUB socket에서 topic을 담은 메시지의 payload part 하나를 수신한다.

```c
char topic[256];
size_t topic_len;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_subscribe_part(s, NULL, topic, sizeof(topic), &topic_len, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `topic_id_buf_`/`topic_id_capacity_`는 caller의 topic buffer와 그
크기다. `topic_id_len_out_`(필수)는 실제 topic 길이를 받는다. `part_out_`(필수, 이미
초기화됨)와 `has_more_out_`(필수)는 `zlink_recv_part`(Raw receive category)와 같은 계약을
따른다. `source_rid_out_`는 선택적이며 raw SUB/XSUB에서는 항상 `NULL`이다.

**Return과 errno.** `zlink_recv_result_t`를 반환한다 — 성공하면 `ZLINK_RECV_OK`이며 topic
바이트가 trailing NUL 없이 caller의 buffer에 복사되고 payload part 소유권이 caller에게
이전된다(정확히 한 번 닫는다 — Message category). Topic buffer가 너무 작으면(`0` 용량
포함) `ZLINK_RECV_BUFFER_TOO_SMALL`과 `ENOBUFS`를 반환하고 필요한 길이만
`topic_id_len_out_`에 쓰며 아무것도 소비하지 않는다 — 더 큰 buffer로 재시도한다. 양수
용량에 `NULL` buffer면 queue를 건드리기 전에 `ZLINK_RECV_INVALID_HANDLE`과 `EFAULT`로
실패한다.

**선택 기준.** 한 multipart message의 모든 payload part를 첫 part부터 마지막까지 같은
스레드에서 이 함수로 수신한다 — `has_more_out_`는 `zlink_recv_part`와 정확히 같은 방식으로
`ZLINK_PART_MORE`와 `ZLINK_PART_FINAL`을 구분한다.

---

## `zlink_subscription_at`

주어진 index의 등록된 구독 필터를 조회한다.

```c
char filter[256];
size_t filter_len = sizeof(filter);
int is_pattern;
zlink_subscription_at(s, 0, filter, &filter_len, &is_pattern);
```

**Parameters.** `index_`는 0부터 시작한다. `filter_out_`/`filter_len_inout_`는 caller의
buffer다 — 진입 시에는 용량, 반환 시에는 실제로 쓰인 길이다. `is_pattern_out_`는 항상
`0`을 보고한다 — 모든 raw 구독은 pattern이 아니라 byte-prefix 필터이기 때문이다.

**Return과 errno.** `zlink_config_result_t`를 반환한다 — 성공하면 `ZLINK_CONFIG_OK`이며
실패 시 부분 데이터를 쓰지 않는다. `index_`가 범위를 벗어나면 `ENOENT`. Buffer가 너무
작으면 `ENOBUFS`(필요한 길이를 `filter_len_inout_`에 쓰고 `is_pattern_out_`는 그대로 두며
inventory도 건드리지 않는다 — 더 큰 buffer로 같은 index를 재시도한다). 구독 조회를
지원하지 않는 handle 타입이면 `ENOTSUP`.

**선택 기준.** 진단 등을 위해 socket의 현재 구독을 나열할 때 쓴다.

---

전체 근거는 [SUB 스펙](../spec/core/socket/03-sub.ko.md)을 참고한다.
