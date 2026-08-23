---
title: "소켓 — PUB"
---

[English](https://zlink-systems.github.io/zlink/spec/core/socket/02-pub/) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: PAIR](01-pair.ko.md) | [다음: SUB](03-sub.ko.md)
<!-- zlink-nav:end -->

# 소켓 -- PUB

> **이 장이 정의하는 것** — PUB 소켓의 발행 동작과 공개 계약.

발행 전용 소켓, 토픽 기반 fan-out. PUB는 송신 전용이며 수신 함수는
적용되지 않습니다.

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
HWM이 찼을 때 `zlink_publish_part()`는 해당 subscriber에 대한 메시지를 버리고
성공을 보고합니다. 송신 큐가 찼을 때 drop 대신 publisher에 backpressure를 주려면
명시적으로 `1`로 설정해야 하며, 이때 `zlink_publish_part()`는
`ZLINK_SUBMIT_BACKPRESSURED`를 반환합니다.

`1`로 설정하면 publisher가 가장 느린 subscriber에 묶입니다. 한 pipe가 차면 같은
socket의 모든 subscriber에 대한 전달이 멈추기 때문입니다. subscriber 속도에
의존하면 안 되는 신뢰 전달은 PUB/SUB가 아니라 request-reply socket이 담당합니다.

## 자동 HWM 기본값

PUB는 context auto HWM 정책에서 `fanout` policy class로 분류됩니다. 활성
auto-HWM profile은 Core memory budget 비율과 역할별 byte 경계를 선택하고, Core는
그 budget을 고유 physical directional queue에 분배합니다. 기본 profile은
`balanced`입니다. 사용자가 `SNDHWM`을 직접 설정하면 그 application 방향은 자동
분배에서 제외됩니다. `SNDBUF`는 OS socket buffer option이며 auto HWM이 변경하지 않습니다.

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

raw `PUB` 또는 `XPUB` 소켓에서 메시지 파트 하나를 발행합니다.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_publish_part (void *subject_,
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
`ZLINK_PART_MORE`로 시작한 멀티파트 메시지는 `ZLINK_PART_FINAL`까지 같은
스레드에서 이 함수로 계속 보내야 하며, 중간에 다른 send helper를 호출하거나
토픽과 플래그를 바꿀 수 없습니다.

이 함수는 성공과 실패 모두에서 `part_`의 내용을 소비합니다. 호출자는 반환값과
관계없이 같은 내용을 다시 보내려면 호출 전에 별도 복사본을 만들어야 하며,
소비된 `zlink_msg_t`를 다시 사용하려면 먼저 초기화해야 합니다.

Core는 성공한 중간 파트를 `ZLINK_PART_FINAL`이 성공할 때까지 하나의 publish
record로 staging합니다. 열린 sequence의 중간 또는 마지막 submit이 실패하면
이전에 staging한 파트와 실패한 파트를 원자적으로 폐기하고 sequence를 닫습니다.
subscriber에는 그 record의 어떤 파트도 보이지 않습니다. 실패한 호출의
`part_`도 소비되며 다음 publish는 새 record의 첫 파트로 시작합니다. 따라서
backpressure를 포함한 실패 뒤에는 보관해 둔 전체 record를 첫 파트부터 다시
제출해야 합니다.

적용 타입은 raw `PUB`, raw `XPUB`입니다. 다른 raw 소켓 타입은
`ZLINK_SUBMIT_NOT_SUPPORTED`를 반환하고 `errno`를 `ENOTSUP`로 설정합니다.

논블로킹 발행은 `flags_`에 `ZLINK_DONTWAIT`를 전달합니다. 즉시 진행할 수
없으면 `ZLINK_SUBMIT_BACKPRESSURED`를 반환합니다. 반환 결과와 관계없이
`part_`가 소비된다는 소유권 규칙은 동일합니다. 전체 결과 대응은
[errno map](../03-errors.ko.md#result와-errno-대응)을 따릅니다.

**참고:** `zlink_publish_part`

---

## Receive flow state

PUB에는 paired DEALER/ROUTER completion lane이 없으므로 receive-flow 상태도 없다.
`zlink_socket_set_receive_flow_state()`는 PUB socket에 대해 `errno == ENOTSUP`과 함께
`ZLINK_CONFIG_NOT_SUPPORTED`를 반환하고 아무것도 바꾸지 않는다. 위에서 설명한 byte HWM,
low water mark와 transport backpressure는 그대로 유지된다. PUB socket의 monitor는
`ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE`를 설정하지 않고
`ZLINK_EVENT_SEND_FLOW_PAUSED`, `ZLINK_EVENT_SEND_FLOW_RESUMED`,
`ZLINK_EVENT_FLOW_STATE_STALE`를 발생시키지 않는다.
