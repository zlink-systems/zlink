---
title: "소켓 — XPUB"
---

[English](04-xpub.en.md) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: SUB](03-sub.ko.md) | [다음: XSUB](05-xsub.ko.md)
<!-- zlink-nav:end -->

# 소켓 -- XPUB

> **이 장이 정의하는 것** — XPUB 소켓(구독 이벤트를 메시지로 노출하는 PUB)의 공개 계약.

구독 전달과 수동 제어를 지원하는 확장 발행자. XPUB는 구독자로부터 구독
이벤트를 수신하고 수동 구독 관리를 지원합니다.

## Pub 옵션 (`zlink_pub_option_t`)

`zlink_set_pub_option()` / `zlink_get_pub_option()`과 함께 사용합니다.

```c
typedef enum zlink_pub_option_t
{
    ZLINK_PUB_OPT_VERBOSE = 0x3301,
    ZLINK_PUB_OPT_VERBOSER = 0x3302,
    ZLINK_PUB_OPT_MANUAL = 0x3303,
    ZLINK_PUB_OPT_MANUAL_LAST_VALUE = 0x3304,
    ZLINK_PUB_OPT_NODROP = 0x3305,
    ZLINK_PUB_OPT_WELCOME_MSG = 0x3306,
    ZLINK_PUB_OPT_TOPICS_COUNT = 0x3307,
    ZLINK_PUB_OPT_APPROVE_SUBSCRIBE = 0x3308,
    ZLINK_PUB_OPT_REJECT_SUBSCRIBE = 0x3309
} zlink_pub_option_t;
```

| 상수 | 설명 |
|---|---|
| `ZLINK_PUB_OPT_VERBOSE` | 모든 구독 메시지를 업스트림 전달 (`int`; 0 또는 1) |
| `ZLINK_PUB_OPT_VERBOSER` | 구독/해제 메시지를 업스트림 전달 (`int`; 0 또는 1) |
| `ZLINK_PUB_OPT_MANUAL` | XPUB 수동 구독 관리 (`int`; 0 또는 1) |
| `ZLINK_PUB_OPT_MANUAL_LAST_VALUE` | 수동 모드 최신 값 캐싱 (`int`; 0 또는 1) |
| `ZLINK_PUB_OPT_NODROP` | HWM 시 drop 대신 `EAGAIN` 반환 (`int`; 0 또는 1, 기본값 `0`) |
| `ZLINK_PUB_OPT_WELCOME_MSG` | 새 subscriber 연결 시 전송 메시지 (`binary`) |
| `ZLINK_PUB_OPT_TOPICS_COUNT` | 구독된 토픽 수 (`int`, 읽기 전용) |
| `ZLINK_PUB_OPT_APPROVE_SUBSCRIBE` | manual 모드 구독 승인 (`binary`) |
| `ZLINK_PUB_OPT_REJECT_SUBSCRIBE` | manual 모드 구독 거부 (`binary`) |

`ZLINK_PUB_OPT_NODROP`의 기본값은 `0`입니다. fanout 전달은 손실을 허용합니다.
HWM(High-Water Mark)에 도달했을 때 `zlink_publish_part()`는 해당 subscriber에 대한
메시지를 버리고 성공을 보고합니다. 송신 큐가 찼을 때 drop 대신 publisher에
backpressure를 주려면 명시적으로 `1`로 설정해야 하며, 이때
`zlink_publish_part()`는 `ZLINK_SUBMIT_BACKPRESSURED`를 반환합니다.

`1`로 설정하면 publisher가 가장 느린 subscriber에 묶입니다. 한 pipe가 차면 같은
socket의 모든 subscriber에 대한 전달이 멈추기 때문입니다. subscriber 속도에
의존하면 안 되는 신뢰 전달은 XPUB/XSUB가 아니라 request-reply socket이 담당합니다.

## 함수

### zlink_set_pub_option

PUB/XPUB 소켓 전용 옵션을 설정합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_pub_option (void *handle_,
                           zlink_pub_option_t option_,
                           const void *optval_,
                           size_t optvallen_);
```

PUB/XPUB 소켓 옵션을 설정합니다. 모든 소켓 타입에 공유되는 공통 옵션은
`zlink_set_option()`을 사용합니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_get_pub_option`, `zlink_set_option`

---

### zlink_get_pub_option

pub 전용 옵션을 조회합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_pub_option (void *handle_,
                           zlink_pub_option_t option_,
                           void *optval_,
                           size_t *optvallen_);
```

PUB/XPUB 소켓 옵션의 현재 값을 가져옵니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_set_pub_option`

---

### zlink_publish_part

raw `XPUB` 소켓에서 메시지 파트 하나를 발행합니다.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_publish_part (
  void *subject_,
  const char *topic_id_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

`topic_id_ == NULL`이면 첫 메시지 프레임이 wire prefix 규칙에 따라 토픽을
운반합니다. NULL이 아니면 `topic_id_`는 NUL로 끝나며 내부 NUL이 없는 바이트
문자열이어야 합니다. 종료 NUL 앞의 모든 바이트가 토픽이며 Core가 이 바이트를
메시지 앞의 토픽 프레임으로 추가합니다.
별도의 토픽 전용 최대 길이는 없으며 토픽 바이트도 메시지와 storage 크기 제한에
포함됩니다. 크기 제한을 넘으면 `ZLINK_SUBMIT_INVALID_ARGUMENT`와 `EMSGSIZE`,
토픽 프레임용 storage를 확보하지 못하면 `ZLINK_SUBMIT_OUT_OF_MEMORY`와 `ENOMEM`을
반환합니다.
`ZLINK_PART_MORE`로 시작한 멀티파트 메시지는 같은 스레드에서 같은 토픽과
플래그를 사용해 `ZLINK_PART_FINAL`까지 이어서 전송합니다.

이 함수는 성공과 실패 모두에서 `part_`의 내용을 소비합니다. 같은 내용을
다시 사용할 가능성이 있으면 호출 전에 복사하고, 소비된 `zlink_msg_t`는
재사용하기 전에 초기화해야 합니다. 논블로킹 발행은 `flags_`에
`ZLINK_DONTWAIT`를 전달하며, 즉시 진행할 수 없으면
`ZLINK_SUBMIT_BACKPRESSURED`를 반환합니다.

Core는 성공한 중간 파트를 `ZLINK_PART_FINAL`이 성공할 때까지 하나의 publish
record로 staging합니다. 열린 sequence의 중간 또는 마지막 submit이 실패하면
이전에 staging한 파트와 실패한 파트를 원자적으로 폐기하고 sequence를 닫습니다.
subscriber에는 그 record의 어떤 파트도 보이지 않습니다. 실패한 호출의
`part_`도 소비되며 다음 publish는 새 record의 첫 파트로 시작합니다. 따라서
backpressure를 포함한 실패 뒤에는 보관해 둔 전체 record를 첫 파트부터 다시
제출해야 합니다.

적용 타입은 raw `PUB`, raw `XPUB`입니다. 다른 타입은
`ZLINK_SUBMIT_NOT_SUPPORTED`, `errno == ENOTSUP`입니다. 전체 결과 대응은
[errno map](../04-errno-map.ko.md)을 따릅니다.

**참고:** `zlink_xpub_recv_part`, `zlink_send_ready_handler`

---

### zlink_xpub_recv_part

XPUB 소켓에서 구독 이벤트를 수신합니다.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_xpub_recv_part (void *xpub_,
                               const zlink_routing_id_t **source_rid_out_,
                               int *subscribed_out_,
                               char *topic_id_buf_,
                               size_t topic_id_capacity_,
                               size_t *topic_id_len_out_,
                               zlink_recv_flags_t flags_);
```

recv 모드에서 다음 구독 이벤트를 수신합니다. 성공 시
`*source_rid_out_`는 구독 피어의 라이브러리 소유 routing ID 포인터로
설정되고(이 소켓에 대한 다음 호출 전까지 유효), `*subscribed_out_`는
subscribe이면 1, unsubscribe이면 0입니다. `topic_id_buf_` /
`*topic_id_len_out_`에 토픽 바이트가 기록됩니다(binary-safe).
호출자는 `topic_id_capacity_`로 버퍼 크기를 전달하며,
토픽이 용량을 초과하면 `errno = EMSGSIZE`로 실패합니다.

적용 대상: raw XPUB만.

**반환값:** 성공 시 `ZLINK_RECV_OK`, 실패 시 `zlink_recv_result_t` 값.
`EAGAIN`/`ETERM` 외의 상세 errno(예: 토픽 용량 초과 `EMSGSIZE`, XPUB가 아닌
subject `EINVAL`)는 `ZLINK_RECV_INTERNAL_ERROR`로 표면화되며, `zlink_errno()`는
진단용 내부 errno를 그대로 유지합니다.

**에러:** `xpub_`가 NULL이면 `EFAULT`. `ZLINK_DONTWAIT`가 설정되고
이벤트가 없으면 `EAGAIN`. 토픽이 `topic_id_capacity_`를 초과하면
`EMSGSIZE`. subject가 XPUB가 아니면 `EINVAL`.

**참고:** `zlink_publish_part`

---

### zlink_send_ready_handler

send-ready 콜백을 설정하거나 교체합니다.

```c
ZLINK_EXPORT zlink_handler_result_t zlink_send_ready_handler (
  void *s_, zlink_send_ready_handler_fn handler_, void *userdata_);
```

핸들러는 교체 전용입니다. NULL 전달은 유효하지 않습니다. 교체 성공 시 다음 쓰기
가능 전환부터 반영됩니다. 동일 핸들의 send-ready 콜백 내에서 재진입 호출하면
`errno=EDEADLK`로 실패합니다.

지원 대상은 raw `PAIR`, `PUB`, `XPUB`, `DEALER`, `ROUTER`, `STREAM`입니다.
send-ready는 수신 모드와 독립적입니다.
이 콜백과 `ZLINK_POLLOUT`은 같은 send-recovery readiness 축을 가리킵니다.
readiness 신호는 송신을 다시 시도할 가치가 있다는 뜻이며, 재시도가 반드시
성공한다는 보장은 아닙니다. 지원하지 않는 subject는 `ENOTSUP`를 반환합니다.

**반환값:** 성공 시 `ZLINK_HANDLER_OK`, 실패 시 `zlink_handler_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_publish_part`
