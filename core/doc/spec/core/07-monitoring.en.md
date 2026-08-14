[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/07-monitoring/) | English

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

typedef void (*zlink_monitor_handler_fn)(
  const zlink_monitor_event_t *event,
  void *userdata);

typedef zlink_monitor_event_t zlink_socket_monitor_event_t;
typedef zlink_monitor_handler_fn zlink_socket_monitor_handler_fn;

#define ZLINK_MONITOR_STATUS_ABI_VERSION 3u

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

`monitor_hwm_bytes` is the single byte budget applied to the monitor source
worker and both directions of its internal monitor PAIR. A positive value is
used unchanged as the exact SNDHWM, RCVHWM, and worker admission limit. Zero is
not unlimited; it selects the Core default computed by
`checkedMultiply(4096, sizeof(socket_monitor_internal_event_t) + sizeof(zlink_msg_t))`.
The worker admits the actual accounted bytes of each event record, not an event
count, and applies only the same one-oversize-record-on-empty rule. Monitor
queues are excluded from application Auto HWM water-filling.
The context budget snapshot counts each unique physical ypipe direction of the
internal monitor PAIR once; it does not add the reader and writer endpoint
option copies separately.

`ZLINK_SOCKET_MONITOR_EVENT_*` names are canonical event-mask names;
`ZLINK_EVENT_*` are shorter names with the same numeric values.
The `ZLINK_DISCONNECT_*` macros are ABI-preserving aliases for the matching
`ZLINK_DISCONNECT_REASON_*` enum values.
`events == 0`
selects no event, while `EVENT_ALL` selects every bit. A raw socket monitor
status has `source_kind == ZLINK_MONITOR_SOURCE_SOCKET`. Optional fields whose
bits are absent from `detail_flags` are zero.
`abi_version` is `ZLINK_MONITOR_STATUS_ABI_VERSION`, and `struct_size` is the
number of bytes in the returned ABI version. Version 3 adds `snd_pending_bytes`
and `rcv_pending_bytes` and removes message-unit, slot, size-cap, and
connection-bucket diagnostic fields. Older layouts are not accepted as
compatibility layouts.

`zlink_socket_monitor_open`, its open-options structure, and its status
structure replace the existing names in place with the current 0.11.1 layout.
There is no caller size/version negotiation or parallel versioned entrypoint.
`abi_version` and `struct_size` diagnose the current layout returned by Core;
they are neither caller inputs nor compatibility-negotiation values. If an HWM
range or calculation or an allocation prevents monitor creation, open fails
with `NULL` and `errno`. Core adds no separate `RESOURCE_LIMIT` config result or
binding error type.

Each detail bit makes exactly the following fields valid. A field belongs to
one bit only, and every field in a row is zero when that bit is absent.

| detail bit | valid fields |
|---|---|
| `ZLINK_MONITOR_STATUS_DETAIL_SND_PENDING_MSGS` | `snd_pending_msgs`, `snd_pending_bytes` |
| `ZLINK_MONITOR_STATUS_DETAIL_RCV_PENDING_MSGS` | `rcv_pending_msgs`, `rcv_pending_bytes` |
| `ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUDGET` | `auto_hwm_enabled`, `auto_hwm_profile`, `auto_hwm_role`, `auto_hwm_policy_class`, `auto_hwm_planned_sndhwm_bytes`, `auto_hwm_planned_rcvhwm_bytes`, `auto_hwm_last_recalc_ms`, `auto_hwm_last_recalc_reason`, `auto_hwm_send_blocked_ratio_ppm`, `auto_hwm_deferred_sndhwm_bytes`, `auto_hwm_deferred_rcvhwm_bytes`, `auto_hwm_deferred_sndhwm_valid`, `auto_hwm_deferred_rcvhwm_valid` |
| `ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUFFERS` | `auto_hwm_applied_sndhwm_bytes`, `auto_hwm_applied_rcvhwm_bytes`, `auto_hwm_effective_sndbuf`, `auto_hwm_effective_rcvbuf`, `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes` |

The planned fields report the current automatic policy result. The applied
fields report the byte HWM currently used by the socket, including manual
overrides. A deferred value is meaningful only when its matching `_valid`
field is non-zero. `snd_bytes_in_flight` and `rcv_bytes_in_flight` are
directional pipe totals at snapshot time. In version 3, `snd_pending_bytes` and
`rcv_pending_bytes` expose those same send and receive in-flight totals. Some
sources estimate the receive total and count. Pending message fields remain
counts. Pending byte fields use the same byte unit as admission accounting, but
the diagnostic values themselves are not admission inputs. The minimum charge
and oversize fields explain byte accounting without requiring an allocator
lookup for every message.

`auto_hwm_send_blocked_ratio_ppm` is the fraction, in parts per million, of
first send-admission attempts in the measurement epoch that were blocked by an
application-pipe HWM. Retries after the same submission wakes are not counted
again; transport-I/O waits, the completion lane, and context-aggregate usage
are excluded.

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

`connection_id` identifies one physical transport attempt within the current
process. When Application and Completion transports form one Framework peer,
physical lifecycle events for each lane use the same `transport_pair_id` and
`transport_pair_generation`, and `transport_lane` distinguishes the lanes.
`CONNECTION_READY`, however, aggregates a pair whose two lanes are ready as one
public ready transport. It emits exactly one ready edge per pair id and
generation and counts that pair once in `value`. Learning the routing ID at
different times on the two lanes does not split the pair into two ready
transports. For an unpaired transport, the pair fields are zero and
`transport_lane` is the Application value. A pair id is not a global identifier
that survives process restarts.

For `CONNECTION_READY`, `value` is the current count of public ready transports
reported by the monitor source. Use
`ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE` in `flags` to identify the
transition that increased the count. A ready-count event without this flag is
a count snapshot, not a new connection-ready edge.

`zlink_socket_monitor_recv` writes the complete current 0.11.1
`zlink_socket_monitor_event_t` layout. The caller must provide an output buffer
for that current layout. Core exposes neither a separate receive entry point
for the previous event prefix nor a version-negotiation path.

## 2. Ordering, overflow, and thread safety

The monitor queue is bounded. When full, it aggregates identical high-frequency
events and prioritizes connection-state, protocol-error, and lifecycle events.
The next status snapshot reflects aggregated counts. A delayed monitor consumer
does not block raw-socket submission.

Within one monitor, Core queues events in the order in which it commits state
transitions. No wall-clock order is guaranteed across connection I/O threads.
Handler and receive follow the single-consumer rule for one event queue. Results
and errno follow the [errno map](04-errno-map.en.md).
