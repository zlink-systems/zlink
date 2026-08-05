[한국어](07-monitoring.ko.md) | English

[Specification index](../README.en.md) · [Core index](README.en.md) · [Events](05-events.en.md) · [errno map](04-errno-map.en.md)

# Monitoring

This document defines the ZLink Core raw-socket monitor contract. Its
audience is C API and binding developers who observe connection, transport,
protocol, and socket lifecycle. A monitor observes state and never changes
routing or queue state.

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

`ZLINK_SOCKET_MONITOR_EVENT_*` names are canonical event-mask names;
`ZLINK_EVENT_*` are shorter names with the same numeric values.
The `ZLINK_DISCONNECT_*` macros are ABI-preserving aliases for the matching
`ZLINK_DISCONNECT_REASON_*` enum values.
`events == 0`
selects no event, while `EVENT_ALL` selects every bit. A raw socket monitor
status has `source_kind == ZLINK_MONITOR_SOURCE_SOCKET`. Optional fields whose
bits are absent from `detail_flags` are zero. When no connection bucket applies,
`auto_hwm_connection_bucket_index` is `UINT32_MAX`.
`abi_version` is `ZLINK_MONITOR_STATUS_ABI_VERSION`, and `struct_size` is the
number of bytes in the returned ABI version. Version 2 replaces the former
32-bit count HWM fields with 64-bit byte fields; the old layout is not accepted
as a compatibility layout.

Each detail bit makes exactly the following fields valid. A field belongs to
one bit only, and every field in a row is zero when that bit is absent.

| detail bit | valid fields |
|---|---|
| `ZLINK_MONITOR_STATUS_DETAIL_SND_PENDING_MSGS` | `snd_pending_msgs` |
| `ZLINK_MONITOR_STATUS_DETAIL_RCV_PENDING_MSGS` | `rcv_pending_msgs` |
| `ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUDGET` | `auto_hwm_enabled`, `auto_hwm_profile`, `auto_hwm_role`, `auto_hwm_policy_class`, `auto_hwm_unit_budget_bytes`, `auto_hwm_size_cap`, `auto_hwm_socket_message_slots`, `auto_hwm_connection_bucket_enabled`, `auto_hwm_connection_bucket_count`, `auto_hwm_connection_bucket_index`, `auto_hwm_connection_bucket_hwm_4k`, `auto_hwm_connection_bucket_hysteresis_retained`, `auto_hwm_effective_message_bytes`, `auto_hwm_planned_sndhwm_bytes`, `auto_hwm_planned_rcvhwm_bytes`, `auto_hwm_last_recalc_ms`, `auto_hwm_last_recalc_reason`, `auto_hwm_send_blocked_ratio_ppm`, `auto_hwm_deferred_sndhwm_bytes`, `auto_hwm_deferred_rcvhwm_bytes`, `auto_hwm_deferred_sndhwm_valid`, `auto_hwm_deferred_rcvhwm_valid` |
| `ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUFFERS` | `auto_hwm_applied_sndhwm_bytes`, `auto_hwm_applied_rcvhwm_bytes`, `auto_hwm_effective_sndbuf`, `auto_hwm_effective_rcvbuf`, `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes` |

The planned fields report the current automatic policy result. The applied
fields report the byte HWM currently used by the socket, including manual
overrides. A deferred value is meaningful only when its matching `_valid`
field is non-zero. `snd_bytes_in_flight` and `rcv_bytes_in_flight` are
directional pipe totals at snapshot time. Pending message fields remain counts.
The minimum charge and oversize fields explain byte accounting without
requiring an allocator lookup for every message.

The socket monitor provides bind, accept, connect, disconnect, handshake,
protocol-error, and close events. Handler mode and receive mode are mutually
exclusive; the second mode returns `EBUSY`. With receive, event addresses and
routing IDs are values inside the caller-owned output structure. A callback's
event pointer and contained values are borrowed views valid only until the
callback returns. `DISCONNECTED.value` is a `zlink_disconnect_reason_t`,
`HANDSHAKE_FAILED_PROTOCOL.value` is a `zlink_protocol_error_t`, and
`PEER_WEIGHT_CHANGED.value` is the new weight in `0..10000`. Other failure-event
values contain the errno for that failure.

`zlink_monitor_ignore_handler()` is a no-op that neither retains nor releases
the event or `userdata`. `event` is a borrowed view valid only for the call.
Registering it through the handler API makes it an ordinary callback consumer
that drains each event without taking further action.

## 2. Ordering, overflow, and thread safety

The monitor queue is bounded. When full, it aggregates identical high-frequency
events and prioritizes connection-state, protocol-error, and lifecycle events.
The next status snapshot reflects aggregated counts. A delayed monitor consumer
does not block raw-socket submission.

Within one monitor, Core queues events in the order in which it commits state
transitions. No wall-clock order is guaranteed across connection I/O threads.
Handler and receive follow the single-consumer rule for one event queue. Results
and errno follow the [errno map](04-errno-map.en.md).
