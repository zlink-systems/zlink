[한국어](03-sub.ko.md) | English

[Spec Index](../../README.en.md) · [Core Index](../README.en.md) · [Socket Common](README.en.md)

# Socket — SUB

Subscribe socket with topic filtering. SUB is receive-only for data;
subscription management is the control plane.

## Automatic HWM defaults

SUB is classified as the `recv_ingress` policy class by the context automatic
HWM policy. The active auto-HWM profile selects the unit budget and
message-size cap; the default profile is `balanced`. Manual `RCVHWM` or
`RCVBUF` settings override the automatic values.

## Sub Options (`zlink_sub_option_t`)

Used with `zlink_set_sub_option()` / `zlink_get_sub_option()`.

```c
typedef enum zlink_sub_option_t
{
    ZLINK_SUB_OPT_TOPICS_COUNT = 0x3400
} zlink_sub_option_t;
```

| Constant | Description |
|---|---|
| `ZLINK_SUB_OPT_TOPICS_COUNT` | Number of subscribed topics (get-only, `int`) |

## Functions

### zlink_set_sub_option

Set a sub-specific option.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_sub_option (void *handle_,
                           zlink_sub_option_t option_,
                           const void *optval_,
                           size_t optvallen_);
```

Configures a SUB/XSUB socket option. Use `zlink_set_option()` for common
options shared across all socket types.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_get_sub_option`, `zlink_set_option`

---

### zlink_get_sub_option

Get a sub-specific option.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_sub_option (void *handle_,
                           zlink_sub_option_t option_,
                           void *optval_,
                           size_t *optvallen_);
```

Retrieves the current value of a SUB/XSUB socket option.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_set_sub_option`

---

### zlink_set_subscription

Subscribe to a topic filter on a raw socket.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_subscription (void *handle_, const char *filter_);
```

Subscribes the handle to messages matching `filter_`. `filter_` is a
NUL-terminated string and cannot contain an embedded NUL. The bytes before the
terminating NUL form a byte-prefix filter: a message matches when its topic
starts with those bytes. An empty string subscribes to all messages. There is
no wildcard syntax; a trailing `*` is matched literally.

Applicable types: raw SUB, raw XSUB.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `EFAULT` if `handle_` is NULL. `EINVAL` if `filter_` is NULL, or the
handle type does not support subscribe.

**See also:** `zlink_unset_subscription`, `zlink_subscribe_part`

---

### zlink_unset_subscription

Unsubscribe from a topic filter on a raw socket.

```c
ZLINK_EXPORT zlink_config_result_t zlink_unset_subscription (void *handle_, const char *filter_);
```

Removes a previously registered subscription. `filter_` must be a
NUL-terminated string without an embedded NUL. The same byte-prefix
interpretation as `zlink_set_subscription()` applies; the bytes before the
terminating NUL must match a previously registered prefix.

Applicable types: raw SUB, raw XSUB.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `EFAULT` if `handle_` is NULL. `EINVAL` if `filter_` is NULL, or the
handle type does not support unsubscribe.

**See also:** `zlink_set_subscription`

---

### zlink_subscribe_part

Receive one payload part of a topic-bearing message from a raw `SUB` or
`XSUB` socket.

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

`topic_id_len_out_`, an initialized `part_out_`, and `has_more_out_` are
required. `source_rid_out_` is optional and always receives `NULL` for raw
`SUB` and `XSUB`. On success, the function copies the binary topic bytes into
the caller's buffer without appending a NUL byte and transfers ownership of
the payload part to the caller. The caller must close the received part
exactly once with `zlink_msg_close(part_out_)`.

When `topic_id_capacity_ == 0` or is too small for the topic, the function
writes the required topic length to `*topic_id_len_out_` and returns
`ZLINK_RECV_BUFFER_TOO_SMALL` with `ENOBUFS`. It does not consume the queued
topic or payload, leaves `part_out_` and every output other than
`topic_id_len_out_` unchanged, and does not transfer part ownership. The caller
can retry the same message with a sufficient buffer. A positive capacity with a
NULL `topic_id_buf_` fails before inspecting or consuming the queue, returns
`ZLINK_RECV_INVALID_HANDLE` with `EFAULT`, and leaves every output and
`part_out_` unchanged.

Receive every payload part from the first through the last part of one
multipart message with this function on the same thread. `*has_more_out_` is
`ZLINK_PART_MORE` when another payload part follows and `ZLINK_PART_FINAL` for
the last part. Applicable types are raw `SUB` and raw `XSUB`.

---

### zlink_subscription_at

Retrieve the subscription filter at a given index.

```c
ZLINK_EXPORT zlink_config_result_t zlink_subscription_at (void *handle_,
                           size_t index_,
                           char *filter_out_,
                           size_t *filter_len_inout_,
                           int *is_pattern_out_);
```

Returns the subscription filter string at `index_` (0-based). On entry,
`*filter_len_inout_` is the buffer size; on return it is set to the actual
length. `*is_pattern_out_` reports whether the filter is a pattern
subscription. All raw subscriptions are byte-prefix filters, so it
always reports `0`.

If the buffer is too small, the function writes the required length to
`*filter_len_inout_` and returns `ZLINK_CONFIG_BUFFER_TOO_SMALL` with
`errno == ENOBUFS`. It writes no partial data to `filter_out_` and leaves
`*is_pattern_out_` unchanged. It does not consume or modify the subscription
inventory, so the caller can retry the same `index_` with a sufficient buffer.

Applicable types: raw SUB, raw XSUB.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `ENOENT` if index is out of range. `ENOBUFS` if the buffer is
too small. `ENOTSUP` if the handle type does not support subscription query.

**See also:** `zlink_set_subscription`, `zlink_get_sub_option`
