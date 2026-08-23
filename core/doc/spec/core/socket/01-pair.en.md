[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/socket/01-pair/) | English

[Spec Index](../../README.en.md) · [Core Index](../README.en.md) · [Socket Common](README.en.md)

# Socket — PAIR

A 1:1 bidirectional socket. Both sides can send and receive messages. PAIR has
no type-specific options.

## Applicable Functions

### zlink_send_part

Send one message part.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part (
  void *s_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

Send a single-part message with `ZLINK_PART_FINAL`. Start a multipart message
with `ZLINK_PART_MORE`, then continue on the same thread with this function and
the same `flags_` through `ZLINK_PART_FINAL`.

This function consumes the content of `part_` on both success and failure. If
the same content may be needed again, copy it before the call. Initialize a
consumed `zlink_msg_t` before reusing it. `flags_` accepts
`ZLINK_SEND_FLAGS_NONE` or `ZLINK_DONTWAIT`. A non-blocking call that cannot
proceed immediately returns `ZLINK_SUBMIT_BACKPRESSURED`.

Core stages successful intermediate parts as one record until
`ZLINK_PART_FINAL` succeeds. If any intermediate or final submit in an open
sequence fails, Core atomically discards the previously staged parts and the
failed part and closes the sequence. No part of that record becomes visible to
the peer. The failed call still consumes `part_` under the rule above, and the
next submit starts the first part of a new record. A retry therefore resubmits
the entire record from its first part using copies retained before the calls.

**Returns:** `ZLINK_SUBMIT_OK` on success; otherwise a
`zlink_submit_result_t` value. See the [errno map](../04-errno-map.en.md) for the
full mapping.

**See also:** `zlink_recv_part`, `zlink_send_async`

---

### zlink_recv_part

Receive one message part.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (
  void *s_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

`part_out_` must be an initialized message and is required together with
`has_more_out_`. `source_rid_out_` is optional and receives `NULL` for PAIR.
On success, ownership of the received part transfers to the caller, which must
call `zlink_msg_close(part_out_)` exactly once. A failure before a part is
received does not transfer ownership.

`*has_more_out_` is `ZLINK_PART_MORE` when another part follows and
`ZLINK_PART_FINAL` for the last part. Receive all parts of one multipart
message on the same thread with this function. The normal path is to observe
`ZLINK_POLLIN` with a poller before calling it. A `ZLINK_DONTWAIT` call with no
available data returns `ZLINK_RECV_NO_DATA`.

**Returns:** `ZLINK_RECV_OK` on success; otherwise a
`zlink_recv_result_t` value.

**See also:** `zlink_send_part`, `zlink_msg_close`

---

### Asynchronous send admission

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_async (
  void *s_, zlink_msg_t *parts_, size_t part_count_,
  const zlink_send_async_options_t *options_,
  zlink_send_op_id_t *op_id_out_);

ZLINK_EXPORT zlink_handler_result_t zlink_send_complete_handler (
  void *s_, zlink_send_complete_handler_fn handler_, void *userdata_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_async_cancel (
  void *s_, zlink_send_op_id_t op_id_);
```

PAIR supports the full asynchronous send admission surface. `options_->target`
is ignored: a PAIR socket has exactly one peer, so all pending operations share
one target queue and are admitted in submit order.

A completion reports admission into the Core send queue, not peer delivery.
[Socket Common](README.en.md) owns the complete contract: ownership transfer,
the per-socket pending bound, per-operation timeout, cancel, close fail-fast,
and the callback rules.

**Returns:** `ZLINK_SUBMIT_OK` / `ZLINK_HANDLER_OK` on success; otherwise a
`zlink_submit_result_t` or `zlink_handler_result_t` value.

**See also:** `zlink_send_part`

## Receive flow state

PAIR has no paired DEALER/ROUTER completion lane, so it has no receive-flow
state. `zlink_socket_set_receive_flow_state()` returns
`ZLINK_CONFIG_NOT_SUPPORTED` with `errno == ENOTSUP` for a PAIR socket and
changes nothing. The byte HWM, low water mark, and transport backpressure
described above stay in effect unchanged, and a monitor for a PAIR socket never
sets `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` or emits
`ZLINK_EVENT_SEND_FLOW_PAUSED`, `ZLINK_EVENT_SEND_FLOW_RESUMED`, or
`ZLINK_EVENT_FLOW_STATE_STALE`.
