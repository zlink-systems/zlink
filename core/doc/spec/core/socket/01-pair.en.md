[한국어](01-pair.ko.md) | English

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

**See also:** `zlink_recv_part`, `zlink_send_ready_handler`

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

### zlink_send_ready_handler

Install or replace the send-ready callback.

```c
ZLINK_EXPORT zlink_handler_result_t zlink_send_ready_handler (
  void *s_, zlink_send_ready_handler_fn handler_, void *userdata_);
```

The handler is replace-only and `NULL` is invalid. A successful replacement is
visible from the next writable transition. Reentrant registration from the
same handle's send-ready callback fails with `ZLINK_HANDLER_DEADLOCK` and
`errno == EDEADLK`.

This callback and `ZLINK_POLLOUT` expose the same send-recovery readiness axis.
The signal means a send retry is worth attempting; it does not guarantee that
the next retry succeeds.

**Returns:** `ZLINK_HANDLER_OK` on success; otherwise a
`zlink_handler_result_t` value.

**See also:** `zlink_send_part`
