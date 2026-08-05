---
title: "Monitoring"
---

[English](07-monitoring.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Polling](06-polling.ko.md) | [다음: Utilities](08-utilities.ko.md)
<!-- zlink-nav:end -->

# Monitoring

> **이 장이 정의하는 것** — `zlink_socket_monitor_*` API로 socket [event](05-events.ko.md)를
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
  ZLINK_SOCKET_MONITOR_EVENT_ALL                        = 0xFFFFu,

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
  ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUFFERS = 1u << 4
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
} zlink_monitor_event_t;

typedef void (*zlink_monitor_handler_fn)(
  const zlink_monitor_event_t *event,
  void *userdata);

typedef zlink_monitor_event_t zlink_socket_monitor_event_t;
typedef zlink_monitor_handler_fn zlink_socket_monitor_handler_fn;

#define ZLINK_MONITOR_STATUS_ABI_VERSION 2u

ZLINK_EXPORT void zlink_monitor_ignore_handler (const zlink_monitor_event_t *event_,
                                                void *userdata_);

typedef struct zlink_socket_monitor_open_options_t {
  zlink_socket_monitor_event_mask_t events;
} zlink_socket_monitor_open_options_t;

typedef struct zlink_monitor_status_t {
  uint32_t abi_version;
  uint32_t struct_size;
  zlink_monitor_source_kind_t source_kind;
  zlink_monitor_state_mask_t state_flags;
  zlink_monitor_status_detail_mask_t detail_flags;
  uint64_t snd_pending_msgs;
  uint64_t rcv_pending_msgs;
  uint32_t auto_hwm_enabled;
  uint32_t auto_hwm_profile;
  uint32_t auto_hwm_role;
  uint32_t auto_hwm_policy_class;
  uint64_t auto_hwm_unit_budget_bytes;
  uint32_t auto_hwm_size_cap;
  uint64_t auto_hwm_socket_message_slots;
  uint32_t auto_hwm_connection_bucket_enabled;
  uint32_t auto_hwm_connection_bucket_count;
  uint32_t auto_hwm_connection_bucket_index;
  uint32_t auto_hwm_connection_bucket_hwm_4k;
  uint32_t auto_hwm_connection_bucket_hysteresis_retained;
  uint64_t auto_hwm_effective_message_bytes;
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

`ZLINK_SOCKET_MONITOR_EVENT_*`가 event mask의 정식 이름이며 `ZLINK_EVENT_*`는 같은 숫자의 짧은 이름이다.
`ZLINK_DISCONNECT_*` macro는 같은 이름의 `ZLINK_DISCONNECT_REASON_*` enum 값에 대한 ABI 유지 alias다.
`events == 0`은 event를 선택하지 않고 `EVENT_ALL`은 모든 bit를 선택한다. raw socket monitor status의
`source_kind`는 `ZLINK_MONITOR_SOURCE_SOCKET`이다. `detail_flags`에 없는 선택 field는 0이며
`auto_hwm_connection_bucket_index`는 bucket이 없으면 `UINT32_MAX`다.
`abi_version`은 `ZLINK_MONITOR_STATUS_ABI_VERSION`이고, `struct_size`는 반환된 ABI version의
전체 byte 크기다. Version 2는 이전 32-bit count HWM field를 64-bit byte field로
교체한다. 이전 layout을 호환 layout으로 받지 않는다.

각 detail bit가 유효하게 만드는 field는 다음과 같다. 한 field는 한 bit에만 속하며 bit가 없으면 표의
field를 모두 0으로 채운다.

| detail bit | 유효한 field |
|---|---|
| `ZLINK_MONITOR_STATUS_DETAIL_SND_PENDING_MSGS` | `snd_pending_msgs` |
| `ZLINK_MONITOR_STATUS_DETAIL_RCV_PENDING_MSGS` | `rcv_pending_msgs` |
| `ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUDGET` | `auto_hwm_enabled`, `auto_hwm_profile`, `auto_hwm_role`, `auto_hwm_policy_class`, `auto_hwm_unit_budget_bytes`, `auto_hwm_size_cap`, `auto_hwm_socket_message_slots`, `auto_hwm_connection_bucket_enabled`, `auto_hwm_connection_bucket_count`, `auto_hwm_connection_bucket_index`, `auto_hwm_connection_bucket_hwm_4k`, `auto_hwm_connection_bucket_hysteresis_retained`, `auto_hwm_effective_message_bytes`, `auto_hwm_planned_sndhwm_bytes`, `auto_hwm_planned_rcvhwm_bytes`, `auto_hwm_last_recalc_ms`, `auto_hwm_last_recalc_reason`, `auto_hwm_send_blocked_ratio_ppm`, `auto_hwm_deferred_sndhwm_bytes`, `auto_hwm_deferred_rcvhwm_bytes`, `auto_hwm_deferred_sndhwm_valid`, `auto_hwm_deferred_rcvhwm_valid` |
| `ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUFFERS` | `auto_hwm_applied_sndhwm_bytes`, `auto_hwm_applied_rcvhwm_bytes`, `auto_hwm_effective_sndbuf`, `auto_hwm_effective_rcvbuf`, `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes` |

Planned field는 현재 자동 정책의 계산 결과를 제공한다. Applied field는 수동 override를
포함해 소켓이 실제로 사용하는 byte HWM을 제공한다. Deferred 값은 대응하는 `_valid`
field가 0이 아닐 때만 유효하다. `snd_bytes_in_flight`와 `rcv_bytes_in_flight`는 snapshot
시점의 directional pipe 합계다. Pending message field의 단위는 계속 count다. Minimum
charge와 oversize field를 사용하면 message마다 allocator를 조회하지 않고도 byte
회계 결과를 진단할 수 있다.

socket monitor는 bind, accept, connect, disconnect, handshake, protocol error와 close event를 제공한다.
handler mode와 recv mode는 상호 배타이며 두 번째 mode는 `EBUSY`다. event의 address와 routing ID는
recv에서는 caller-owned output 구조체 안의 값이다. callback의 event pointer와 그 안의 값은 callback이
반환될 때까지만 유효한 borrowed view다. `DISCONNECTED`의 `value`는 `zlink_disconnect_reason_t`,
`HANDSHAKE_FAILED_PROTOCOL`의 `value`는 `zlink_protocol_error_t`, `PEER_WEIGHT_CHANGED`의 `value`는
새 `0..10000` weight다. 다른 실패 event의 `value`는 해당 실패의 errno다.

`zlink_monitor_ignore_handler()`는 전달된 event와 `userdata`를 보관하거나 해제하지 않는 no-op 함수다.
`event`는 호출 동안만 유효한 borrowed view다. 이 함수를 handler API에 등록하면 일반 callback
consumer처럼 event queue를 소비하되 각 event에 아무 작업도 하지 않는다.

## 2. Ordering, overflow와 thread safety

Monitor queue는 bounded다. Queue가 가득 차면 동일한 high-frequency event를 aggregate하고 connection state,
protocol error와 lifecycle event를 우선 보존한다. Aggregate된 수는 다음 status snapshot에 반영한다. Monitor
consumer 지연은 raw socket submit을 block하지 않는다.

같은 monitor에서는 Core가 state transition을 commit한 순서로 event를 queue에 넣는다. 서로 다른 connection
I/O thread 사이의 wall-clock order는 보장하지 않는다. Handler, recv와 close는 같은 event queue의 single
consumer 규칙을 지킨다. Result와 errno는 [errno map](04-errno-map.ko.md)을 따른다.
