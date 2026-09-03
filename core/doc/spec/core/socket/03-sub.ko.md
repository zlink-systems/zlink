---
title: "Socket — SUB"
---

[English](https://zlink-systems.github.io/zlink/spec/core/socket/03-sub/) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: PUB](02-pub.ko.md) | [다음: XPUB](04-xpub.ko.md)
<!-- zlink-nav:end -->

# Socket — SUB

> **이 장이 정의하는 것** — SUB socket의 구독 동작과 공개 계약.

## 1. SUB socket 개요

SUB는 topic 필터링으로 구독한 message만 받는 구독 전용
[socket](../glossary.ko.md#socket) 타입이다. message가 함께 운반하는 분류용 byte 열을
topic이라 하며, SUB는 등록한 구독 filter와 topic이 매칭되는 message를 수신한다.
SUB는 data 수신 전용이다 — 발행은 [PUB](02-pub.ko.md)·[XPUB](04-xpub.ko.md)가 담당하고,
구독 등록·해제 같은 구독 관리는 message를 나르는 data plane이 아니라 socket을
설정·제어하는 control plane 호출이다.

이 문서는 SUB 고유 계약을 정의한다 — 구독 filter의 등록·해제와 매칭 규칙, SUB 전용
옵션(`zlink_sub_option_t`), topic part 수신 함수와 구독 목록 조회. 이 함수들은 raw
XSUB에도 적용되며, XSUB 고유 동작은 [XSUB](05-xsub.ko.md)가 정의한다.

관련 계약의 소유 문서는 다음과 같다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| socket 공통 옵션(`RCVHWM`·`RCVBUF` 포함)·수명·스레드 안전성·수신 모델 | [Socket 공통](README.ko.md) |
| 구독 message를 발행하는 socket과 topic wire 규칙 | [PUB](02-pub.ko.md), [XPUB](04-xpub.ko.md) |
| Auto HWM budget 계산·admission | [Auto HWM](../systems/06-auto-hwm.ko.md) |
| message lifecycle과 ownership | [Message](../02-message.ko.md) |

## 2. 구독과 filter 매칭

구독은 filter 문자열 단위로 관리한다.

1. **등록** — [`zlink_set_subscription`](#zlink_set_subscription)이 filter 하나를
   구독으로 등록한다. filter의 종료 NUL 앞 byte 열이 byte-prefix가 되어, topic이
   그 byte로 시작하는 message가 매칭된다. 빈 문자열은 모든 message를 구독한다.
   wildcard 구문은 없다.
2. **해제** — [`zlink_unset_subscription`](#zlink_unset_subscription)이 같은
   byte-prefix 해석으로 이전에 등록한 구독을 제거한다.
3. **조회** — 구독된 topic 수는 읽기 전용 옵션
   [`ZLINK_SUB_OPT_TOPICS_COUNT`](#5-옵션-zlink_sub_option_t)로 읽고, 개별 filter는
   [`zlink_subscription_at`](#zlink_subscription_at)으로 index를 지정해 읽는다.
4. **수신** — 매칭된 message는 [`zlink_subscribe_part`](#zlink_subscribe_part)로
   topic과 payload part를 받는다.

## 3. 자동 HWM 기본값

SUB의 수신 queue가 유지할 byte 상한([HWM](../glossary.ko.md#hwm))은 application이 직접
정하지 않으면 context의 [Auto HWM](../glossary.ko.md#auto-hwm-budget) 정책이 자동으로
계산한다.

SUB는 context auto HWM 정책에서 `recv_ingress` 역할로 분류된다. 활성
auto-HWM profile은 Core memory budget 비율과 역할별 byte 경계를 선택하고, Core는
그 budget을 고유 physical [directional queue](../glossary.ko.md#directional-queue)에
분배한다. 기본 profile은 `balanced`다. 사용자가 `RCVHWM`을 직접 설정하면 그
application 방향은 자동 분배에서 제외된다. `RCVBUF`는 OS socket buffer option이며
auto HWM이 변경하지 않는다.

budget 계산과 admission의 정확한 계약은 [Auto HWM](../systems/06-auto-hwm.ko.md)이,
`RCVHWM`·`RCVBUF` 옵션 자체는 [Socket 공통](README.ko.md#transportbuffer)이 소유한다.

## 4. Receive flow state

DEALER와 ROUTER는 자신에게 보내는 peer에게 receive-flow 상태를 알린다. SUB은 receive-flow
대상 socket type이 아니다.

- `zlink_socket_set_receive_flow_state()`는 SUB socket에 대해 `errno == ENOTSUP`과
  함께 `ZLINK_CONFIG_NOT_SUPPORTED`를 반환하고 아무것도 바꾸지 않는다.
- [Socket 공통](README.ko.md#transportbuffer)이 정의하는 byte HWM, low water mark와
  transport [backpressure](../glossary.ko.md#backpressure)는 그대로 유지된다.
- SUB socket의 monitor는 `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE`를 설정하지 않고
  `ZLINK_EVENT_SEND_FLOW_PAUSED`, `ZLINK_EVENT_SEND_FLOW_RESUMED`,
  `ZLINK_EVENT_FLOW_STATE_STALE`를 발생시키지 않는다.

## 5. 옵션 (`zlink_sub_option_t`)

`zlink_set_sub_option()` / `zlink_get_sub_option()`과 함께 사용한다.

```c
typedef enum zlink_sub_option_t
{
    ZLINK_SUB_OPT_TOPICS_COUNT = 0x3400  // 구독된 topic 수 (int, 읽기 전용)
} zlink_sub_option_t;
```

## 6. 함수

### zlink_set_sub_option

SUB/XSUB socket 전용 옵션을 설정한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_sub_option (void *handle_,
                           zlink_sub_option_t option_,
                           const void *optval_,
                           size_t optvallen_);
```

SUB/XSUB socket 옵션을 설정한다. 모든 socket 타입에 공유되는 공통 옵션은
`zlink_set_option()`을 사용한다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_get_sub_option`, `zlink_set_option`

---

### zlink_get_sub_option

SUB/XSUB socket 전용 옵션을 조회한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_sub_option (void *handle_,
                           zlink_sub_option_t option_,
                           void *optval_,
                           size_t *optvallen_);
```

SUB/XSUB socket 옵션의 현재 값을 가져온다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_set_sub_option`

---

### zlink_set_subscription

topic filter를 구독한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_subscription (void *handle_, const char *filter_);
```

`filter_`에 매칭되는 message를 구독한다. `filter_`는 NUL로 끝나는 문자열이며
내부 NUL을 포함할 수 없다. 종료 NUL 앞의 byte를 byte-prefix filter로
사용하므로 message topic이 그 byte로 시작하면 매칭된다. 빈 문자열은 모든
message를 구독한다. wildcard 구문은 없으며 후행 `*`도 literal byte다.

적용 대상: raw SUB, raw XSUB.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**에러:** `handle_`이 NULL이면 `EFAULT`. `filter_`가 NULL이거나 handle 타입이
구독을 지원하지 않으면 `EINVAL`.

**참고:** `zlink_unset_subscription`, `zlink_subscribe_part`

---

### zlink_unset_subscription

topic filter 구독을 해제한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_unset_subscription (void *handle_, const char *filter_);
```

이전에 등록된 구독을 제거한다. `filter_`는 NUL로 끝나고 내부 NUL이 없는
문자열이어야 한다. `zlink_set_subscription()`과 같은 byte-prefix 해석을
사용하며 종료 NUL 앞의 byte가 이전에 등록한 prefix와 일치해야 한다.

적용 대상: raw SUB, raw XSUB.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**에러:** `handle_`이 NULL이면 `EFAULT`. `filter_`가 NULL이거나 handle 타입이
구독 해제를 지원하지 않으면 `EINVAL`.

**참고:** `zlink_set_subscription`

---

### zlink_subscribe_part

raw `SUB` 또는 `XSUB` socket에서 topic message의 payload part 하나를
수신한다.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_subscribe_part (void *sub_,
                                                       const zlink_routing_id_t **source_rid_out_,
                                                       char *topic_id_buf_,
                                                       size_t topic_id_capacity_,
                                                       size_t *topic_id_len_out_,
                                                       zlink_msg_t *part_out_,
                                                       zlink_part_flag_t *has_more_out_,
                                                       zlink_recv_flags_t flags_);
```

`topic_id_len_out_`, 초기화된 `part_out_`, `has_more_out_`은 필수다.
`source_rid_out_`은 선택 사항이며 raw `SUB`와 `XSUB`에서는 항상 `NULL`을
받는다. 성공하면 topic의 binary byte를 호출자 buffer에 NUL 없이 복사하고
payload part의 소유권을 호출자에게 이전한다. 호출자는 받은 part를
`zlink_msg_close(part_out_)`로 정확히 한 번 닫아야 한다.

`topic_id_capacity_ == 0`이거나 topic을 담기에 작으면 함수는
`*topic_id_len_out_`에 필요한 topic 길이를 기록하고
`ZLINK_RECV_BUFFER_TOO_SMALL`과 `ENOBUFS`를 반환한다. 이 경우 queue의 topic과
payload를 소비하지 않으며 `topic_id_len_out_`을 제외한 output과 `part_out_`은
변경하지 않는다. part 소유권도 이전하지 않으므로 호출자는 충분한 buffer로
같은 message를 다시 수신할 수 있다. 용량이 0보다 큰데 `topic_id_buf_`가
NULL이면 queue를 검사하거나 소비하기 전에 `ZLINK_RECV_INVALID_HANDLE`과
`EFAULT`를 반환하고 모든 output과 `part_out_`을 변경하지 않는다.

한 multipart message의 첫 payload part부터 마지막 part까지 같은 thread에서 이
함수로 계속 수신해야 한다. `*has_more_out_`은 다음 payload part가 있으면
`ZLINK_PART_MORE`, 마지막이면 `ZLINK_PART_FINAL`이다. 적용 타입은 raw
`SUB`, raw `XSUB`다.

---

### zlink_subscription_at

지정된 index의 구독 filter를 조회한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_subscription_at (void *handle_,
                           size_t index_,
                           char *filter_out_,
                           size_t *filter_len_inout_,
                           int *is_pattern_out_);
```

`index_`는 조회 시점의 구독 snapshot을 filter byte 열의 사전식 오름차순으로
정렬한 결과에 대한 0-기반 index이며 등록 순서가 아니다. 호출이 성공하면
`filter_out_`에는 해당 filter byte만 기록하고 종료 NUL은 추가하지 않는다.
따라서 이 output은 C 문자열이 아니며, 진입 시 `*filter_len_inout_`는 buffer
크기이고 반환 시 filter byte 길이다. `is_pattern_out_`은 NULL을 허용하는
선택 output이다. NULL이 아니면 filter가 pattern 구독인지 기록하며,
모든 raw 구독은 byte-prefix filter이므로 `0`을 기록한다.

buffer가 작으면 필요한 길이를 `*filter_len_inout_`에 기록하고
`ZLINK_CONFIG_BUFFER_TOO_SMALL`, `errno == ENOBUFS`를 반환한다. 이 결과에서는
`filter_out_`에 부분 데이터를 기록하지 않고 `*is_pattern_out_`도 변경하지 않는다.
구독 목록(subscription inventory)을 소비하거나 변경하지 않으므로 호출자는 같은
`index_`를 충분한 buffer로 다시 조회할 수 있다.

적용 타입: raw SUB, raw XSUB.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**에러:** index가 범위를 벗어나면 `ENOENT`. buffer가 작으면 `ENOBUFS`.
handle 타입이 구독 조회를 지원하지 않으면 `ENOTSUP`.

**참고:** `zlink_set_subscription`, `zlink_get_sub_option`

## 7. 구현 및 contract test 검증 요구

공개 표면(구독 함수, SUB 옵션 set·get, `zlink_subscribe_part` 결과, 반환값·errno)만으로
다음을 확인한다. 각 항목은 unit test 하나로 이어진다.

**구독 등록·해제**
- `zlink_set_subscription`으로 등록한 filter는 byte-prefix로 매칭된다 — topic이 filter의 종료 NUL 앞 byte로 시작하는 message가 `zlink_subscribe_part`로 수신된다.
- 빈 문자열 filter는 모든 message를 구독한다.
- 후행 `*`를 포함한 filter는 `*`를 literal byte로 매칭한다 — wildcard로 확장되지 않는다.
- `zlink_unset_subscription`은 종료 NUL 앞 byte가 이전에 등록한 prefix와 일치하는 구독을 제거한다.
- `zlink_set_subscription`·`zlink_unset_subscription`에서 `handle_`이 NULL이면 `EFAULT`, `filter_`가 NULL이거나 handle 타입이 구독(해제)을 지원하지 않으면 `EINVAL`이다.

**구독 목록 조회**
- 읽기 전용 `ZLINK_SUB_OPT_TOPICS_COUNT`를 `zlink_get_sub_option`으로 읽으면 구독된 topic 수가 `int`로 반환된다.
- `zlink_subscription_at`의 0-기반 `index_`는 등록 순서가 아니라 filter byte 열로 정렬한 snapshot 순서를 따른다.
  이 snapshot은 호출마다 새로 만들며 `ZLINK_SUB_OPT_TOPICS_COUNT` 조회와 원자적으로 묶이지 않는다. 두 호출 사이에
  subscription이 바뀌면 같은 `index_`가 다른 filter를 가리키거나 `ENOENT`가 될 수 있으므로, 목록을 읽는 동안에는
  caller가 subscription 변경을 직렬화한다.
- 성공 시 `filter_out_`에는 filter byte만 기록되고 종료 NUL은 기록되지 않는다. `*filter_len_inout_`은 그 byte 길이며 `filter_out_`은 C 문자열이 아니다.
- `is_pattern_out_`은 NULL을 허용하는 선택 output이다. 제공하면 모든 raw 구독이 byte-prefix filter이므로 `0`이 기록된다.
- buffer가 작으면 필요한 길이를 `*filter_len_inout_`에 기록하고 `ZLINK_CONFIG_BUFFER_TOO_SMALL`과 `ENOBUFS`를 반환한다. `filter_out_`에 부분 데이터를 쓰지 않고 `*is_pattern_out_`도 바꾸지 않으며, 같은 `index_`를 충분한 buffer로 다시 조회할 수 있다.
- 범위를 벗어난 index는 `ENOENT`, 구독 조회를 지원하지 않는 handle 타입은 `ENOTSUP`이다.

**topic part 수신**
- 성공한 `zlink_subscribe_part`는 topic의 binary byte를 NUL 없이 호출자 buffer에 복사하고 payload part의 소유권을 호출자에게 이전한다 — 호출자가 `zlink_msg_close(part_out_)`를 정확히 한 번 호출한다.
- raw SUB·XSUB에서 `source_rid_out_`은 항상 `NULL`을 받는다.
- `topic_id_capacity_`가 0이거나 topic보다 작으면 `*topic_id_len_out_`에 필요한 길이를 기록하고 `ZLINK_RECV_BUFFER_TOO_SMALL`과 `ENOBUFS`를 반환한다. queue의 topic과 payload는 소비되지 않아 충분한 buffer로 다시 호출하면 같은 message를 수신하고, `topic_id_len_out_`을 제외한 output과 `part_out_`은 변하지 않는다.
- 용량이 0보다 큰데 `topic_id_buf_`가 NULL이면 queue를 검사·소비하기 전에 `ZLINK_RECV_INVALID_HANDLE`과 `EFAULT`를 반환하고 모든 output과 `part_out_`이 변하지 않는다.
- multipart message는 `*has_more_out_`이 `ZLINK_PART_MORE`인 동안 다음 payload part가 이어지고 마지막 part에서 `ZLINK_PART_FINAL`이 된다.

**자동 HWM 기본값**
- `RCVHWM`을 직접 설정한 application 방향은 자동 분배에서 제외된다.
- `RCVBUF`는 auto HWM이 변경하지 않는다.

**Receive flow state 부재**
- SUB socket에 `zlink_socket_set_receive_flow_state()`를 호출하면 `ZLINK_CONFIG_NOT_SUPPORTED`와 `ENOTSUP`이며 아무것도 바뀌지 않는다.
- SUB socket의 monitor는 `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE`를 설정하지 않고 `ZLINK_EVENT_SEND_FLOW_PAUSED`·`ZLINK_EVENT_SEND_FLOW_RESUMED`·`ZLINK_EVENT_FLOW_STATE_STALE`를 발생시키지 않는다.

**공통 반환 규약**
- `zlink_config_result_t`를 반환하는 위 함수들은 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값을 반환하며 `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

Auto HWM budget 계산·admission의 검증은
[Auto HWM §5](../systems/06-auto-hwm.ko.md#5-구현-및-contract-test-검증-요구)가 소유한다.
