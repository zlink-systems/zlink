---
title: "소켓 — SUB"
---

[English](03-sub.en.md) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: PUB](02-pub.ko.md) | [다음: XPUB](04-xpub.ko.md)
<!-- zlink-nav:end -->

# 소켓 -- SUB

> **이 장이 정의하는 것** — SUB 소켓의 구독 동작과 공개 계약.

토픽 필터링을 사용하는 구독 소켓. SUB는 데이터 수신 전용이며,
구독 관리는 control plane에 해당합니다.

## 자동 HWM 기본값

SUB는 context auto HWM 정책에서 `recv_ingress` policy class로 분류됩니다.
활성 auto-HWM profile이 단위 예산과 메시지 크기 cap을 고르며, 기본 profile은
`balanced`입니다. 사용자가 `RCVHWM`이나 `RCVBUF`를 직접 설정하면 자동값보다
그 값이 우선합니다.

## Sub 옵션 (`zlink_sub_option_t`)

`zlink_set_sub_option()` / `zlink_get_sub_option()`과 함께 사용합니다.

```c
typedef enum zlink_sub_option_t
{
    ZLINK_SUB_OPT_TOPICS_COUNT = 0x3400
} zlink_sub_option_t;
```

| 상수 | 설명 |
|---|---|
| `ZLINK_SUB_OPT_TOPICS_COUNT` | 구독된 토픽 수 (읽기 전용, `int`) |

## 함수

### zlink_set_sub_option

SUB/XSUB 소켓 전용 옵션을 설정합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_sub_option (void *handle_,
                           zlink_sub_option_t option_,
                           const void *optval_,
                           size_t optvallen_);
```

SUB/XSUB 소켓 옵션을 설정합니다. 모든 소켓 타입에 공유되는 공통 옵션은
`zlink_set_option()`을 사용합니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_get_sub_option`, `zlink_set_option`

---

### zlink_get_sub_option

SUB/XSUB 소켓 전용 옵션을 조회합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_sub_option (void *handle_,
                           zlink_sub_option_t option_,
                           void *optval_,
                           size_t *optvallen_);
```

SUB/XSUB 소켓 옵션의 현재 값을 가져옵니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_set_sub_option`

---

### zlink_set_subscription

토픽 필터를 구독합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_subscription (void *handle_, const char *filter_);
```

`filter_`에 매칭되는 메시지를 구독합니다. `filter_`는 NUL로 끝나는 문자열이며
내부 NUL을 포함할 수 없습니다. 종료 NUL 앞의 바이트를 byte-prefix 필터로
사용하므로 메시지 토픽이 그 바이트로 시작하면 매칭됩니다. 빈 문자열은 모든
메시지를 구독합니다. wildcard 구문은 없으며 후행 `*`도 리터럴 바이트입니다.

적용 대상: raw SUB, raw XSUB.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:** `handle_`이 NULL이면 `EFAULT`. `filter_`가 NULL이거나 handle 타입이
구독을 지원하지 않으면 `EINVAL`.

**참고:** `zlink_unset_subscription`, `zlink_subscribe_part`

---

### zlink_unset_subscription

토픽 필터 구독을 해제합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_unset_subscription (void *handle_, const char *filter_);
```

이전에 등록된 구독을 제거합니다. `filter_`는 NUL로 끝나고 내부 NUL이 없는
문자열이어야 합니다. `zlink_set_subscription()`과 같은 byte-prefix 해석을
사용하며 종료 NUL 앞의 바이트가 이전에 등록한 prefix와 일치해야 합니다.

적용 대상: raw SUB, raw XSUB.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:** `handle_`이 NULL이면 `EFAULT`. `filter_`가 NULL이거나 handle 타입이
구독 해제를 지원하지 않으면 `EINVAL`.

**참고:** `zlink_set_subscription`

---

### zlink_subscribe_part

raw `SUB` 또는 `XSUB` 소켓에서 토픽 메시지의 payload 파트 하나를
수신합니다.

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

`topic_id_len_out_`, 초기화된 `part_out_`, `has_more_out_`은 필수입니다.
`source_rid_out_`은 선택 사항이며 raw `SUB`와 `XSUB`에서는 항상 `NULL`을
받습니다. 성공하면 토픽의 binary byte를 호출자 버퍼에 NUL 없이 복사하고
payload 파트의 소유권을 호출자에게 이전합니다. 호출자는 받은 파트를
`zlink_msg_close(part_out_)`로 정확히 한 번 닫아야 합니다.

`topic_id_capacity_ == 0`이거나 토픽을 담기에 작으면 함수는
`*topic_id_len_out_`에 필요한 토픽 길이를 기록하고
`ZLINK_RECV_BUFFER_TOO_SMALL`과 `ENOBUFS`를 반환합니다. 이 경우 queue의 토픽과
payload를 소비하지 않으며 `topic_id_len_out_`을 제외한 output과 `part_out_`은
변경하지 않습니다. 파트 소유권도 이전하지 않으므로 호출자는 충분한 버퍼로
같은 메시지를 다시 수신할 수 있습니다. 용량이 0보다 큰데 `topic_id_buf_`가
NULL이면 queue를 검사하거나 소비하기 전에 `ZLINK_RECV_INVALID_HANDLE`과
`EFAULT`를 반환하고 모든 output과 `part_out_`을 변경하지 않습니다.

한 멀티파트 메시지의 첫 payload 파트부터 마지막 파트까지 같은 스레드에서 이
함수로 계속 수신해야 합니다. `*has_more_out_`은 다음 payload 파트가 있으면
`ZLINK_PART_MORE`, 마지막이면 `ZLINK_PART_FINAL`입니다. 적용 타입은 raw
`SUB`, raw `XSUB`입니다.

---

### zlink_subscription_at

지정된 인덱스의 구독 필터를 조회한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_subscription_at (void *handle_,
                           size_t index_,
                           char *filter_out_,
                           size_t *filter_len_inout_,
                           int *is_pattern_out_);
```

`index_` (0-기반)에 해당하는 구독 필터 문자열을 반환합니다. 진입 시
`*filter_len_inout_`는 버퍼 크기이며, 반환 시 실제 길이로 설정됩니다.
`*is_pattern_out_`는 필터가 패턴 구독인지 보고하며, 모든 raw 구독이 byte-prefix
필터이므로 항상 `0`을 반환합니다.

버퍼가 작으면 필요한 길이를 `*filter_len_inout_`에 기록하고
`ZLINK_CONFIG_BUFFER_TOO_SMALL`, `errno == ENOBUFS`를 반환합니다. 이 결과에서는
`filter_out_`에 부분 데이터를 기록하지 않고 `*is_pattern_out_`도 변경하지 않습니다.
subscription inventory를 소비하거나 변경하지 않으므로 호출자는 같은 `index_`를
충분한 버퍼로 다시 조회할 수 있습니다.

적용 타입: raw SUB, raw XSUB.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:** 인덱스가 범위를 벗어나면 `ENOENT`. 버퍼가 작으면 `ENOBUFS`.
handle 타입이 구독 조회를 지원하지 않으면 `ENOTSUP`.

**참고:** `zlink_set_subscription`, `zlink_get_sub_option`
