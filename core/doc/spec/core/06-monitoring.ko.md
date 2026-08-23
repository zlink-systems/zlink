---
title: "Monitoring"
---

[English](06-monitoring.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Polling](05-polling.ko.md) | [다음: Utilities](07-utilities.ko.md)
<!-- zlink-nav:end -->

# Monitoring

> **이 장이 정의하는 것** — `zlink_socket_monitor_*` API로 socket [event](04-events.ko.md)를
> 별도 채널로 구독하는 공개 계약.

이 문서는 ZLink Core raw socket monitor 공개 계약을 정의한다. 대상 독자는 connection, transport,
protocol과 socket lifecycle을 관측하는 C API와 bindings 개발자다. Monitor는 상태를 관측할 뿐 routing과
queue 상태를 변경하지 않는다.

## 1. Raw socket monitor

```c
typedef enum zlink_monitor_source_kind_t {
  ZLINK_MONITOR_SOURCE_SOCKET = 1
} zlink_monitor_source_kind_t;

typedef uint32_t zlink_socket_monitor_event_mask_t;

typedef enum zlink_socket_monitor_event_e {
  ZLINK_SOCKET_MONITOR_EVENT_CONNECTED                  = 1u << 0,
  ZLINK_SOCKET_MONITOR_EVENT_CONNECT_DELAYED            = 1u << 1,
  ZLINK_SOCKET_MONITOR_EVENT_CONNECT_RETRIED            = 1u << 2,
  ZLINK_SOCKET_MONITOR_EVENT_LISTENING                  = 1u << 3,
  ZLINK_SOCKET_MONITOR_EVENT_BIND_FAILED                = 1u << 4,
  ZLINK_SOCKET_MONITOR_EVENT_ACCEPTED                   = 1u << 5,
  ZLINK_SOCKET_MONITOR_EVENT_ACCEPT_FAILED              = 1u << 6,
  ZLINK_SOCKET_MONITOR_EVENT_CLOSED                     = 1u << 7,
  ZLINK_SOCKET_MONITOR_EVENT_CLOSE_FAILED               = 1u << 8,
  ZLINK_SOCKET_MONITOR_EVENT_DISCONNECTED               = 1u << 9,
  ZLINK_SOCKET_MONITOR_EVENT_MONITOR_STOPPED            = 1u << 10,
  ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_NO_DETAIL = 1u << 11,
  ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY           = 1u << 12,
  ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_PROTOCOL  = 1u << 13,
  ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_AUTH      = 1u << 14,
  ZLINK_SOCKET_MONITOR_EVENT_PEER_WEIGHT_CHANGED        = 1u << 15,
  ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_PAUSED           = 1u << 16,
  ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_RESUMED          = 1u << 17,
  ZLINK_SOCKET_MONITOR_EVENT_FLOW_STATE_STALE           = 1u << 18,
  ZLINK_SOCKET_MONITOR_EVENT_ALL                        = 0x7FFFFu,

  ZLINK_EVENT_CONNECTED                  = ZLINK_SOCKET_MONITOR_EVENT_CONNECTED,
  ZLINK_EVENT_CONNECT_DELAYED            = ZLINK_SOCKET_MONITOR_EVENT_CONNECT_DELAYED,
  ZLINK_EVENT_CONNECT_RETRIED            = ZLINK_SOCKET_MONITOR_EVENT_CONNECT_RETRIED,
  ZLINK_EVENT_LISTENING                  = ZLINK_SOCKET_MONITOR_EVENT_LISTENING,
  ZLINK_EVENT_BIND_FAILED                = ZLINK_SOCKET_MONITOR_EVENT_BIND_FAILED,
  ZLINK_EVENT_ACCEPTED                   = ZLINK_SOCKET_MONITOR_EVENT_ACCEPTED,
  ZLINK_EVENT_ACCEPT_FAILED              = ZLINK_SOCKET_MONITOR_EVENT_ACCEPT_FAILED,
  ZLINK_EVENT_CLOSED                     = ZLINK_SOCKET_MONITOR_EVENT_CLOSED,
  ZLINK_EVENT_CLOSE_FAILED               = ZLINK_SOCKET_MONITOR_EVENT_CLOSE_FAILED,
  ZLINK_EVENT_DISCONNECTED               = ZLINK_SOCKET_MONITOR_EVENT_DISCONNECTED,
  ZLINK_EVENT_MONITOR_STOPPED            = ZLINK_SOCKET_MONITOR_EVENT_MONITOR_STOPPED,
  ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL =
    ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_NO_DETAIL,
  ZLINK_EVENT_CONNECTION_READY           = ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY,
  ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL  =
    ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_PROTOCOL,
  ZLINK_EVENT_HANDSHAKE_FAILED_AUTH      =
    ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_AUTH,
  ZLINK_EVENT_PEER_WEIGHT_CHANGED        =
    ZLINK_SOCKET_MONITOR_EVENT_PEER_WEIGHT_CHANGED,
  ZLINK_EVENT_SEND_FLOW_PAUSED           =
    ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_PAUSED,
  ZLINK_EVENT_SEND_FLOW_RESUMED          =
    ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_RESUMED,
  ZLINK_EVENT_FLOW_STATE_STALE           =
    ZLINK_SOCKET_MONITOR_EVENT_FLOW_STATE_STALE,
  ZLINK_EVENT_ALL                        = ZLINK_SOCKET_MONITOR_EVENT_ALL
} zlink_socket_monitor_event_e;

typedef uint32_t zlink_monitor_state_mask_t;

typedef enum zlink_monitor_state_flag_e {
  ZLINK_MONITOR_STATE_READY       = 1u << 0,
  ZLINK_MONITOR_STATE_BOUND_READY = 1u << 1,
  ZLINK_MONITOR_STATE_CLOSED      = 1u << 3
} zlink_monitor_state_flag_e;

typedef uint32_t zlink_monitor_status_detail_mask_t;

typedef enum zlink_monitor_status_detail_flag_e {
  ZLINK_MONITOR_STATUS_DETAIL_SND_PENDING_MSGS = 1u << 1,
  ZLINK_MONITOR_STATUS_DETAIL_RCV_PENDING_MSGS = 1u << 2,
  ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUDGET  = 1u << 3,
  ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUFFERS = 1u << 4,
  ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE       = 1u << 5
} zlink_monitor_status_detail_flag_e;

typedef enum zlink_auto_hwm_recalc_reason_t {
  ZLINK_AUTO_HWM_RECALC_REASON_NONE            = 0,
  ZLINK_AUTO_HWM_RECALC_REASON_INITIAL         = 1,
  ZLINK_AUTO_HWM_RECALC_REASON_ROLE_CHANGE     = 2,
  ZLINK_AUTO_HWM_RECALC_REASON_POLICY_TOGGLE   = 3,
  ZLINK_AUTO_HWM_RECALC_REASON_REFRESH         = 4,
  ZLINK_AUTO_HWM_RECALC_REASON_DEFERRED_SHRINK = 5
} zlink_auto_hwm_recalc_reason_t;

typedef enum zlink_disconnect_reason_t {
  ZLINK_DISCONNECT_REASON_UNKNOWN          = 0,
  ZLINK_DISCONNECT_REASON_HANDSHAKE_FAILED = 3,
  ZLINK_DISCONNECT_REASON_TRANSPORT_ERROR  = 4,
  ZLINK_DISCONNECT_REASON_CTX_TERM         = 5
} zlink_disconnect_reason_t;

#define ZLINK_DISCONNECT_UNKNOWN ZLINK_DISCONNECT_REASON_UNKNOWN
#define ZLINK_DISCONNECT_HANDSHAKE_FAILED ZLINK_DISCONNECT_REASON_HANDSHAKE_FAILED
#define ZLINK_DISCONNECT_TRANSPORT_ERROR ZLINK_DISCONNECT_REASON_TRANSPORT_ERROR
#define ZLINK_DISCONNECT_CTX_TERM ZLINK_DISCONNECT_REASON_CTX_TERM

typedef enum zlink_protocol_error_t {
  ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_HELLO = 0x10000013
} zlink_protocol_error_t;

typedef struct zlink_monitor_event_t {
  uint64_t event;
  uint64_t value;
  zlink_routing_id_t routing_id;
  char local_addr[256];
  char remote_addr[256];
  uint64_t connection_id;
  uint64_t transport_pair_id;
  uint64_t transport_pair_generation;
  uint32_t transport_lane;
  uint32_t flags;
} zlink_monitor_event_t;

typedef enum zlink_monitor_transport_lane_e {
  ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION = 0,
  ZLINK_MONITOR_TRANSPORT_LANE_COMPLETION  = 1
} zlink_monitor_transport_lane_t;

#define ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE (1u << 0)
#define ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE (1u << 1)
#define ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION (1u << 2)
#define ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH (1u << 3)

typedef void (*zlink_monitor_handler_fn)(
  const zlink_monitor_event_t *event,
  void *userdata);

typedef zlink_monitor_event_t zlink_socket_monitor_event_t;
typedef zlink_monitor_handler_fn zlink_socket_monitor_handler_fn;

#define ZLINK_MONITOR_STATUS_ABI_VERSION 4u

ZLINK_EXPORT void zlink_monitor_ignore_handler (const zlink_monitor_event_t *event_,
                                                void *userdata_);

typedef struct zlink_socket_monitor_open_options_t {
  zlink_socket_monitor_event_mask_t events;
  uint64_t monitor_hwm_bytes;
} zlink_socket_monitor_open_options_t;

typedef struct zlink_monitor_status_t {
  uint32_t abi_version;
  uint32_t struct_size;
  zlink_monitor_source_kind_t source_kind;
  zlink_monitor_state_mask_t state_flags;
  zlink_monitor_status_detail_mask_t detail_flags;
  uint64_t snd_pending_msgs;
  uint64_t rcv_pending_msgs;
  uint64_t snd_pending_bytes;
  uint64_t rcv_pending_bytes;
  uint32_t auto_hwm_enabled;
  uint32_t auto_hwm_profile;
  uint32_t auto_hwm_role;
  uint32_t auto_hwm_policy_class;
  uint64_t auto_hwm_planned_sndhwm_bytes;
  uint64_t auto_hwm_planned_rcvhwm_bytes;
  uint64_t auto_hwm_applied_sndhwm_bytes;
  uint64_t auto_hwm_applied_rcvhwm_bytes;
  int32_t auto_hwm_effective_sndbuf;
  int32_t auto_hwm_effective_rcvbuf;
  uint64_t auto_hwm_last_recalc_ms;
  uint32_t auto_hwm_last_recalc_reason;
  uint32_t auto_hwm_send_blocked_ratio_ppm;
  uint64_t auto_hwm_deferred_sndhwm_bytes;
  uint64_t auto_hwm_deferred_rcvhwm_bytes;
  uint32_t auto_hwm_deferred_sndhwm_valid;
  uint32_t auto_hwm_deferred_rcvhwm_valid;
  uint64_t snd_bytes_in_flight;
  uint64_t rcv_bytes_in_flight;
  uint64_t minimum_core_message_charge_bytes;
  uint64_t oversize_message_admission_count;
  uint64_t oversize_message_admission_max_bytes;
  uint64_t flow_paused_connections;
  uint64_t flow_pause_applied_total;
  uint64_t flow_resume_applied_total;
  uint64_t flow_state_stale_total;
  uint64_t flow_pause_duration_ms;
} zlink_monitor_status_t;

ZLINK_EXPORT void *zlink_socket_monitor_open(
  void *socket,
  const zlink_socket_monitor_open_options_t *options);
ZLINK_EXPORT zlink_handler_result_t zlink_socket_monitor_handler(
  void *monitor,
  zlink_socket_monitor_handler_fn handler,
  void *userdata);
ZLINK_EXPORT zlink_recv_result_t zlink_socket_monitor_recv(
  void *monitor,
  zlink_socket_monitor_event_t *event_out,
  zlink_recv_flags_t flags);
ZLINK_EXPORT zlink_config_result_t zlink_monitor_status(
  void *monitor,
  zlink_monitor_status_t *status_out);
ZLINK_EXPORT zlink_close_result_t zlink_monitor_close(void **monitor_p);
```

`monitor_hwm_bytes`는 monitor source worker와 내부 monitor PAIR 양쪽에 적용하는
단일 byte 예산이다. 양수는 변환 없이 정확한 SNDHWM·RCVHWM과 worker admission
상한으로 사용한다. `0`은 unlimited가 아니라 Core가
`checkedMultiply(4096, sizeof(socket_monitor_internal_event_t) + sizeof(zlink_msg_t))`
로 계산한 기본 byte 값을 선택한다. Worker도 event count가 아니라 실제 record의
accounted byte로 수용 여부를 판단하고, 빈 queue의 oversize record 한 건 규칙만
동일하게 적용한다. Monitor queue는 application Auto HWM water-filling에서 제외한다.
Context budget snapshot은 내부 monitor PAIR의 고유한 physical ypipe 방향을 한 번씩만
집계하며 reader와 writer endpoint의 option을 중복해서 더하지 않는다.

`ZLINK_SOCKET_MONITOR_EVENT_*`가 event mask의 정식 이름이며 `ZLINK_EVENT_*`는 같은 숫자의 짧은 이름이다.
`ZLINK_DISCONNECT_*` macro는 같은 이름의 `ZLINK_DISCONNECT_REASON_*` enum 값에 대한 ABI 유지 alias다.
`events == 0`은 event를 선택하지 않고 `EVENT_ALL`은 모든 bit를 선택한다. raw socket monitor status의
`source_kind`는 `ZLINK_MONITOR_SOURCE_SOCKET`이다. `detail_flags`에 없는 선택 field는 0이며
`abi_version`은 `ZLINK_MONITOR_STATUS_ABI_VERSION`이고, `struct_size`는 반환된 ABI version의
전체 byte 크기다. Version 3은 `snd_pending_bytes`와 `rcv_pending_bytes`를 추가하고
message-unit, slot, size-cap과 connection-bucket 진단 field를 제거한다. Version 4는 구조체
끝에 receive-flow field 5개를 덧붙이고 다른 변경은 없다. 이전 layout을
호환 layout으로 받지 않는다.

`zlink_socket_monitor_open`, open options와 status 구조체는 0.13.0의 현재 layout으로 기존
이름에서 교체한다. Caller size/version 협상이나 병렬 versioned entrypoint를 추가하지 않는다.
`abi_version`과 `struct_size`는 Core가 반환한 현재 layout의 진단값이며 caller 입력이나 호환성
협상값이 아니다. HWM 범위·계산 또는 allocation 때문에 monitor를 열 수 없으면 open은 `NULL`과
`errno`로 실패한다. 별도 `RESOURCE_LIMIT` config result나 binding error type을 추가하지 않는다.

각 detail bit가 유효하게 만드는 field는 다음과 같다. 한 field는 한 bit에만 속하며 bit가 없으면 표의
field를 모두 0으로 채운다.

| detail bit | 유효한 field |
|---|---|
| `ZLINK_MONITOR_STATUS_DETAIL_SND_PENDING_MSGS` | `snd_pending_msgs`, `snd_pending_bytes` |
| `ZLINK_MONITOR_STATUS_DETAIL_RCV_PENDING_MSGS` | `rcv_pending_msgs`, `rcv_pending_bytes` |
| `ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUDGET` | `auto_hwm_enabled`, `auto_hwm_profile`, `auto_hwm_role`, `auto_hwm_policy_class`, `auto_hwm_planned_sndhwm_bytes`, `auto_hwm_planned_rcvhwm_bytes`, `auto_hwm_last_recalc_ms`, `auto_hwm_last_recalc_reason`, `auto_hwm_send_blocked_ratio_ppm`, `auto_hwm_deferred_sndhwm_bytes`, `auto_hwm_deferred_rcvhwm_bytes`, `auto_hwm_deferred_sndhwm_valid`, `auto_hwm_deferred_rcvhwm_valid` |
| `ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUFFERS` | `auto_hwm_applied_sndhwm_bytes`, `auto_hwm_applied_rcvhwm_bytes`, `auto_hwm_effective_sndbuf`, `auto_hwm_effective_rcvbuf`, `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes` |
| `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` | `flow_paused_connections`, `flow_pause_applied_total`, `flow_resume_applied_total`, `flow_state_stale_total`, `flow_pause_duration_ms` |

Planned field는 현재 자동 정책의 계산 결과를 제공한다. Applied field는 수동 override를
포함해 소켓이 실제로 사용하는 byte HWM을 제공한다. Deferred 값은 대응하는 `_valid`
field가 0이 아닐 때만 유효하다. `snd_bytes_in_flight`와 `rcv_bytes_in_flight`는 snapshot
시점의 directional pipe 합계다. 현재 version 3에서 `snd_pending_bytes`와
`rcv_pending_bytes`는 각각 같은 send·receive in-flight 합계를 제공한다. Receive 합계와
count는 일부 source에서 근삿값이다. Pending message field의 단위는 계속 count이며
pending byte field는 admission 회계와 같은 byte 단위를 사용하지만 진단값 자체를
admission 입력으로 사용하지 않는다. Minimum charge와 oversize field를 사용하면
message마다 allocator를 조회하지 않고도 byte 회계 결과를 진단할 수 있다.

`auto_hwm_send_blocked_ratio_ppm`은 측정 epoch에서 해당 socket의 최초 send admission
시도 중 application pipe HWM 때문에 block된 비율이다. 같은 submit의 wake 뒤 재시도는
다시 세지 않으며 transport I/O wait, completion lane과 context aggregate 사용량은 제외한다.

`ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE`는 paired DEALER/ROUTER completion lane을 가진
socket 유형에서만 설정한다. 다른 socket 유형에서는 이 bit가 없고 field 5개가 모두 0이다.
이 field는 해당 socket이 peer에서 관측한 receive-flow 상태를 제공한다.

| Field | 종류 | 의미 |
|---|---|---|
| `flow_paused_connections` | gauge | 이 socket이 현재 remote-PAUSED로 보는 application pipe 수. 적용된 PAUSED 전이마다 1 증가하고, 짝이 되는 RUNNING 전이 또는 PAUSED 상태에서 종료된 pipe마다 1 감소한다. |
| `flow_pause_applied_total` | counter | Socket 생성 이후 실제로 적용된 PAUSED 전이 수. Stale·중복·같은 상태 frame은 세지 않는다. |
| `flow_resume_applied_total` | counter | 같은 규칙으로 실제 적용된 RUNNING 전이 수. PAUSED 상태에서 종료된 pipe는 resume으로 세지 않는다. |
| `flow_state_stale_total` | counter | Stale이나 중복으로 판정해 무시한 flow-state frame 수. |
| `flow_pause_duration_ms` | duration | 이 socket에서 가장 최근에 끝난 PAUSED 구간의 길이(밀리초). 완료된 구간이 없으면 0이다. Pipe 종료로 끝난 pause도 길이를 기록한다. |

Counter 3개는 socket 수명 동안 단조 증가한다. 이 값을 reset하거나 rebase하는 공개 호출은
없으며 `zlink_ctx_reset_auto_hwm_budget_metrics`도 이 값을 바꾸지 않는다. Snapshot 일관성
규칙은 그대로다. 이 field는 같은 호출의 다른 모든 field와 같은 snapshot 경계에서 읽는다.

socket monitor는 bind, accept, connect, disconnect, handshake, protocol error와 close event를 제공한다.
handler mode와 recv mode는 상호 배타이며 두 번째 mode는 `EBUSY`다. event의 address와 routing ID는
recv에서는 caller-owned output 구조체 안의 값이다. callback의 event pointer와 그 안의 값은 callback이
반환될 때까지만 유효한 borrowed view다. `DISCONNECTED`의 `value`는 `zlink_disconnect_reason_t`,
`HANDSHAKE_FAILED_PROTOCOL`의 `value`는 `zlink_protocol_error_t`, `PEER_WEIGHT_CHANGED`의 `value`는
새 `0..10000` weight다. 다른 실패 event의 `value`는 해당 실패의 errno다.

`zlink_monitor_ignore_handler()`는 전달된 event와 `userdata`를 보관하거나 해제하지 않는 no-op 함수다.
`event`는 호출 동안만 유효한 borrowed view다. 이 함수를 handler API에 등록하면 일반 callback
consumer처럼 event queue를 소비하되 각 event에 아무 작업도 하지 않는다.

`connection_id`는 현재 프로세스에서 하나의 물리적 transport 시도를 식별한다. Application과
Completion transport가 한 Framework peer를 구성하는 경우 각 lane의 물리 lifecycle event는 같은
`transport_pair_id`와 `transport_pair_generation`을 사용하고 `transport_lane`으로 lane을 구분한다.
단, `CONNECTION_READY`는 두 lane이 모두 준비된 pair를 하나의 공개 ready transport로 집계하므로 같은
pair id와 generation마다 ready edge를 정확히 한 번만 발생시키고 `value`에도 한 번만 센다. 두 lane이
서로 다른 시점에 routing ID를 알게 되어도 pair를 두 ready transport로 나누지 않는다. Pair를 사용하지
않는 transport에서는 pair field가 0이며 `transport_lane`은 Application 값이다. Pair id는 프로세스
재시작 사이에 유지되는 전역 식별자가 아니다.

`CONNECTION_READY`의 `value`는 해당 monitor source가 ready인 공개 transport 수의 현재 count다. Count가
증가한 순간을 구분해야 하면 `flags`의 `ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE`를 사용한다.
이 flag가 없는 ready count event는 count snapshot이며 새로운 연결의 ready edge를 뜻하지 않는다.

`ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE`,
`ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION`,
`ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH`는 receive-flow event 3개에만 적용한다.
발생 조건과 각 event의 `value` 의미는 [Events](04-events.ko.md)가 소유한다.

`zlink_socket_monitor_recv`는 현재 0.13.0 `zlink_socket_monitor_event_t` layout 전체를 기록한다.
호출자는 현재 layout 크기의 output buffer를 제공해야 하며, 이전 event prefix를 위한 별도 receive
entry point나 version 협상 경로는 제공하지 않는다.

## 2. Ordering, overflow와 thread safety

Monitor queue는 bounded다. Queue가 가득 차면 동일한 high-frequency event를 aggregate하고 connection state,
protocol error와 lifecycle event를 우선 보존한다. Aggregate된 수는 다음 status snapshot에 반영한다. Monitor
consumer 지연은 raw socket submit을 block하지 않는다.

같은 monitor에서는 Core가 state transition을 commit한 순서로 event를 queue에 넣는다. 서로 다른 connection
I/O thread 사이의 wall-clock order는 보장하지 않는다. Handler, recv와 close는 같은 event queue의 single
consumer 규칙을 지킨다. Result와 errno는 [errno map](03-errors.ko.md#result와-errno-대응)을 따른다.
