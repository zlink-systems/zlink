[한국어](07-router.ko.md) | English

[Specification index](../../README.en.md) · [Core index](../README.en.md) · [Socket overview](README.en.md) · [errno map](../04-errno-map.en.md)

# Socket — ROUTER

ROUTER is an asynchronous raw socket that manages multiple peer pipes on one
socket and selects a send target by routing ID. It processes ordinary directed
messages and request/reply records.

## 1. Public types

The following numbers are public ABI values.

```c
typedef enum zlink_router_option_t {
  ZLINK_ROUTER_OPT_MANDATORY          = 0x3101,
  ZLINK_ROUTER_OPT_PROBE              = 0x3103,
  ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID = 0x3104,
  ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS = 0x3105,
  ZLINK_ROUTER_OPT_WEIGHT             = 0x3106
} zlink_router_option_t;

typedef enum zlink_part_flag_t {
  ZLINK_PART_FINAL = 0,
  ZLINK_PART_MORE  = 1
} zlink_part_flag_t;

typedef void (*zlink_reply_handler_fn)(
  zlink_request_result_t result_,
  zlink_msg_t *parts_,
  size_t part_count_,
  void *userdata_);

typedef void (*zlink_completion_control_handler_fn)(
  const zlink_routing_id_t *source_rid_,
  zlink_msg_t *parts_,
  size_t part_count_,
  void *userdata_);
```

`ZLINK_PART_MORE` means that another part follows in the same multipart
record. `ZLINK_PART_FINAL` means that the current part is the last part.
Receive APIs use the same values for `has_more_out_`.

## 2. ROUTER options

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_router_option(
  void *handle_,
  zlink_router_option_t option_,
  const void *optval_,
  size_t optvallen_);

ZLINK_EXPORT zlink_config_result_t zlink_get_router_option(
  void *handle_,
  zlink_router_option_t option_,
  void *optval_,
  size_t *optvallen_);
```

| Constant | Value format | Meaning |
|---|---|---|
| `ZLINK_ROUTER_OPT_MANDATORY` | `int`, `0` or `1` | When set to `1`, a directed submit to a routing ID without a connected pipe fails with `ZLINK_SUBMIT_NOT_CONNECTED`; the default is `1` |
| `ZLINK_ROUTER_OPT_PROBE` | `int`, `0` or `1` | Sends an empty raw message when a connection is established so the peer can observe the connection and routing ID; the default is `0` |
| `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID` | Variable-length byte string, set only | Sets the local alias that identifies the pipe created by the next `zlink_connect()`; set it before each connect |
| `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` | Nonnegative `int`, milliseconds | Selects the default timeout used by a request API when `timeout_ms_ == 0`; the default is `5000` |
| `ZLINK_ROUTER_OPT_WEIGHT` | `int`, `0..10000` | Advertises this ROUTER's weight to connected peers; the default is `100` |

For `zlink_get_router_option()`, `*optvallen_` is the input capacity of
`optval_`. On success it is updated to the number of bytes written. HWM,
reconnect and timeout options that are not ROUTER-specific use
`zlink_set_option()` and `zlink_get_option()`.

## 3. Raw receive record classification

The ROUTER receive API does not return `zlink_dealer_message_type_t`. The
following output combinations distinguish an ordinary raw record from a
request record that requires a reply.

| Record | `source_node_rid_out_` | `request_seq_out_` |
|---|---|---:|
| Ordinary raw multipart | Sending peer's routing ID | `0` |
| Received request | Sending peer's routing ID | A nonzero reply sequence |

Replies and terminal failures for work started by
`zlink_router_request_part()` are delivered through the
`zlink_reply_handler_fn` completion and are not returned as ordinary receive
records.

`source_node_rid_out_` is a thread-local view owned by Core. The caller does
not release it and copies its value if it must be retained beyond the next raw
receive call. Starting the next receive call on the same thread invalidates
the preceding view. Every part of one multipart record returns the same
routing ID and request sequence.

## 4. Part sequences and ownership

`*_part` send calls form one multipart sequence from `ZLINK_PART_MORE` through
`ZLINK_PART_FINAL`. While a sequence is open, another send-helper family or a
different routing ID cannot be interleaved on the same handle.

When `part_` points to a valid initialized message, a send API consumes its
message content on both success and failure. The caller therefore cannot read
the pre-submit payload or submit the same content again after the call,
regardless of its result. Payload needed for a retry must be retained in a
separate message before the call.

Each send-helper family stages successful intermediate parts as one record
until `ZLINK_PART_FINAL` succeeds. If an intermediate or final submit in an
open sequence fails, Core atomically discards the previously staged parts and
the failed part and closes the sequence. No part of that record becomes visible
to the peer. The failed call still consumes `part_`, and the next submit starts
the first part of a new record. A failed request submit creates no request
sequence and invokes no handler. After a reply-sequence failure, the reply-token
and peer-RID pair remains valid until a successful `ZLINK_PART_FINAL` or request-lifecycle
termination, so the caller can resubmit a retained complete reply from its
first part.

`part_out_` passed to a receive API must be an initialized `zlink_msg_t`. On
success, ownership of the received part moves to the caller, which closes it
exactly once with `zlink_msg_close()`. No received-part ownership moves on
failure.

## 5. Directed raw send

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid(
  void *s_,
  const zlink_routing_id_t *target_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

This sends one ordinary raw multipart part to the peer identified by
`target_rid_`. Every part uses the same target. `flags_` is
`ZLINK_SEND_FLAGS_NONE` or `ZLINK_SEND_FLAGS_DONTWAIT`. This API creates no
request sequence or completion handler.

## 6. Raw request submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_router_request_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_);
```

This submits an asynchronous request payload to `peer_rid_` one part at a
time. For an intermediate part, pass `ZLINK_PART_MORE`, `timeout_ms_ == 0`,
`handler_ == NULL`, and `userdata_ == NULL`. The last part uses
`ZLINK_PART_FINAL` and a non-null `handler_`. A final call with `timeout_ms_ ==
0` uses the `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` default.

If the final submit returns `ZLINK_SUBMIT_OK`, exactly one completion is
delivered to `handler_`. A failed submit does not invoke the handler. Ownership
of callback `parts_` and every message moves to the callback, which releases
them exactly once.

## 7. Raw request and message receive

```c
ZLINK_EXPORT zlink_recv_result_t zlink_router_recv_part(
  void *router_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

This returns one part from a complete raw record. Every output pointer is
required. `flags_` is `ZLINK_RECV_FLAGS_NONE` or
`ZLINK_RECV_FLAGS_DONTWAIT`. A non-blocking call with no available record
returns `ZLINK_RECV_NO_DATA` with `EAGAIN`.

When `has_more_out_ == ZLINK_PART_MORE`, the next call receives the next part
of the same record. `ZLINK_PART_FINAL` completes that record's receive
sequence. Use the output combinations in section 3 to determine whether a
reply is required.

## 8. Raw reply submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_router_reply_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  uint64_t request_seq_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

This sends a reply part for a request returned by
`zlink_router_recv_part()`. `peer_rid_` and the nonzero `request_seq_` are the
values returned by that receive record. A multipart reply uses the same two
values for every part. A successful `ZLINK_PART_FINAL` completes the reply.

## 9. Raw completion control

```c
ZLINK_EXPORT zlink_handler_result_t
zlink_router_completion_control_handler(
  void *router_,
  zlink_completion_control_handler_fn handler_,
  void *userdata_);

ZLINK_EXPORT zlink_submit_result_t
zlink_router_completion_control_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

A completion control is a bounded raw multipart record whose contents Core
does not interpret. It uses the Completion connection in the existing
Application/Completion connection pair. It creates no socket or connection,
and ordinary directed messages and requests remain on the Application
connection.

Each socket has one handler, and a later registration replaces it. A null
handler returns `ZLINK_HANDLER_INVALID_ARGUMENT`; a non-ROUTER socket returns
`ZLINK_HANDLER_NOT_SUPPORTED`. A record received without a registered handler
is discarded.

Closing the same socket while the callback is running returns
`ZLINK_CLOSE_BUSY` with `EBUSY`. Close can be retried after the callback
returns.

The handler runs when the completion owner processes the connection. A
`ZLINK_POLLCOMPLETION` poller can therefore receive controls without calling an
application receive API. `source_rid_` remains valid only until the callback
returns. Ownership of every payload part moves to the callback, which releases
or consumes each part exactly once.

Part sequencing and failure ownership follow section 4. Every submit call
consumes its supplied `part_` on every result. The Completion connection has a
finite byte HWM. When submit returns `ZLINK_SUBMIT_BACKPRESSURED`, the caller
keeps independent copies and retries the complete record from its first part
after send-ready. Core defines no command kind, allowlist, or application
meaning for the payload.

## 10. Results and readiness

Submit APIs return `zlink_submit_result_t`, receive APIs return
`zlink_recv_result_t`, and option APIs return `zlink_config_result_t`. The
[errno map](../04-errno-map.en.md) defines the mapping between each result and
`zlink_errno()`.

ROUTER `ZLINK_POLLIN` means that a complete raw record can be received.
`ZLINK_POLLOUT` and `zlink_send_ready_handler()` indicate that retrying a
backpressured submit is worthwhile; they do not guarantee that the next submit
will succeed.
