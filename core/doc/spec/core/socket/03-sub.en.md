---
title: "Socket — SUB"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/socket/03-sub/) | English

<!-- zlink-nav:start -->
[Socket Index](README.en.md) | [Previous: PUB](02-pub.en.md) | [Next: XPUB](04-xpub.en.md)
<!-- zlink-nav:end -->

# Socket — SUB

> **What this chapter defines** — the subscription behavior and public contract of a SUB socket.

## 1. SUB socket overview

SUB is a receive-only [socket](../glossary.en.md#socket) type that uses topic filtering to receive
only subscribed messages. A topic is a sequence of classification bytes carried with a message,
and SUB receives a message when its topic matches a registered subscription filter. SUB receives
data only—[PUB](02-pub.en.md) and [XPUB](04-xpub.en.md) publish it. Subscription management, such
as registering and removing subscriptions, uses control-plane calls that configure and control the
socket rather than the data plane that carries messages.

This document defines the SUB-specific contract: registering and removing subscription filters,
filter matching rules, SUB-specific options (`zlink_sub_option_t`), the topic-part receive function,
and subscription inventory queries. These functions also apply to raw XSUB; [XSUB](05-xsub.en.md)
defines XSUB-specific behavior.

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| Common socket options (including `RCVHWM` and `RCVBUF`), lifetime, thread safety, and receive model | [Socket Common](README.en.md) |
| Sockets that publish subscription messages and topic wire rules | [PUB](02-pub.en.md), [XPUB](04-xpub.en.md) |
| Auto HWM budget calculation and admission | [Auto HWM](../systems/06-auto-hwm.en.md) |
| Message lifecycle and ownership | [Message](../02-message.en.md) |

## 2. Subscriptions and filter matching

Subscriptions are managed by filter string.

1. **Register** — [`zlink_set_subscription`](#zlink_set_subscription) registers one filter as a
   subscription. The bytes before the terminating NUL form a byte prefix, and a message matches
   when its topic starts with those bytes. An empty string subscribes to every message. There is no
   wildcard syntax.
2. **Remove** — [`zlink_unset_subscription`](#zlink_unset_subscription) removes a previously
   registered subscription using the same byte-prefix interpretation.
3. **Query** — Read the number of subscribed topics through the read-only
   [`ZLINK_SUB_OPT_TOPICS_COUNT`](#5-options-zlink_sub_option_t) option, and read an individual
   filter by index through [`zlink_subscription_at`](#zlink_subscription_at).
4. **Receive** — Receive the topic and payload parts of a matching message through
   [`zlink_subscribe_part`](#zlink_subscribe_part).

## 3. Automatic HWM defaults

Unless the application sets it directly, the context [Auto HWM budget](../glossary.en.md#auto-hwm-budget)
policy automatically calculates the byte limit ([HWM](../glossary.en.md#hwm)) retained by the SUB
receive queue.

SUB is classified as the `recv_ingress` policy class by the context Auto HWM policy. The active
Auto HWM profile selects the Core memory-budget ratio and the per-role byte boundaries, and Core
distributes that budget among unique physical [directional queues](../glossary.en.md#directional-queue).
The default profile is `balanced`. If the user sets `RCVHWM` directly, that application direction is
excluded from automatic distribution. `RCVBUF` is an OS socket-buffer option and is not changed by
Auto HWM.

[Auto HWM](../systems/06-auto-hwm.en.md) owns the exact budget-calculation and admission contract.
[Socket Common](README.en.md#transportbuffer) owns the `RCVHWM` and `RCVBUF` options themselves.

## 4. Receive flow state

DEALER and ROUTER report receive-flow state to peers that send to them. SUB is not a socket type that
supports receive flow.

- `zlink_socket_set_receive_flow_state()` returns `ZLINK_CONFIG_NOT_SUPPORTED` with
  `errno == ENOTSUP` for a SUB socket and changes nothing.
- The byte HWM, low water mark, and transport [backpressure](../glossary.en.md#backpressure) defined
  by [Socket Common](README.en.md#transportbuffer) remain in effect.
- A SUB socket monitor does not set `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` and does not emit
  `ZLINK_EVENT_SEND_FLOW_PAUSED`, `ZLINK_EVENT_SEND_FLOW_RESUMED`, or
  `ZLINK_EVENT_FLOW_STATE_STALE`.

## 5. Options (`zlink_sub_option_t`)

Use these options with `zlink_set_sub_option()` / `zlink_get_sub_option()`.

```c
typedef enum zlink_sub_option_t
{
    ZLINK_SUB_OPT_TOPICS_COUNT = 0x3400  // Number of subscribed topics (int, read-only)
} zlink_sub_option_t;
```

## 6. Functions

### zlink_set_sub_option

Set a SUB/XSUB-specific socket option.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_sub_option (void *handle_,
                           zlink_sub_option_t option_,
                           const void *optval_,
                           size_t optvallen_);
```

Configures a SUB/XSUB socket option. Use `zlink_set_option()` for common options shared across all
socket types.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_get_sub_option`, `zlink_set_option`

---

### zlink_get_sub_option

Get a SUB/XSUB-specific socket option.

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

Subscribe to a topic filter.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_subscription (void *handle_, const char *filter_);
```

Subscribes to messages matching `filter_`. `filter_` is a NUL-terminated string and cannot contain
an embedded NUL. The bytes before the terminating NUL form a byte-prefix filter, so a message
matches when its topic starts with those bytes. An empty string subscribes to every message. There
is no wildcard syntax; a trailing `*` is a literal byte.

Applicable types: raw SUB, raw XSUB.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `EFAULT` if `handle_` is NULL. `EINVAL` if `filter_` is NULL or the handle type does not
support subscriptions.

**See also:** `zlink_unset_subscription`, `zlink_subscribe_part`

---

### zlink_unset_subscription

Unsubscribe from a topic filter.

```c
ZLINK_EXPORT zlink_config_result_t zlink_unset_subscription (void *handle_, const char *filter_);
```

Removes a previously registered subscription. `filter_` must be a NUL-terminated string without an
embedded NUL. It uses the same byte-prefix interpretation as `zlink_set_subscription()`; the bytes
before the terminating NUL must match a previously registered prefix.

Applicable types: raw SUB, raw XSUB.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `EFAULT` if `handle_` is NULL. `EINVAL` if `filter_` is NULL or the handle type does not
support removing subscriptions.

**See also:** `zlink_set_subscription`

---

### zlink_subscribe_part

Receive one payload part of a topic-bearing message from a raw `SUB` or `XSUB` socket.

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

`topic_id_len_out_`, an initialized `part_out_`, and `has_more_out_` are required.
`source_rid_out_` is optional and always receives `NULL` for raw `SUB` and `XSUB`. On success, the
function copies the binary topic bytes into the caller's buffer without a NUL byte and transfers
ownership of the payload part to the caller. The caller must close the received part exactly once
with `zlink_msg_close(part_out_)`.

If `topic_id_capacity_ == 0` or is too small for the topic, the function writes the required topic
length to `*topic_id_len_out_` and returns `ZLINK_RECV_BUFFER_TOO_SMALL` with `ENOBUFS`. It does not
consume the queued topic or payload, leaves `part_out_` and every output other than
`topic_id_len_out_` unchanged, and does not transfer ownership of the part. The caller can retry the same
message with a sufficient buffer. If capacity is greater than zero but `topic_id_buf_` is NULL, the
function returns `ZLINK_RECV_INVALID_HANDLE` with `EFAULT` before inspecting or consuming the queue
and leaves every output and `part_out_` unchanged.

Receive every payload part from the first through the last part of one multipart message with this
function on the same thread. `*has_more_out_` is `ZLINK_PART_MORE` when another payload part follows
and `ZLINK_PART_FINAL` for the last part. Applicable types are raw `SUB` and raw `XSUB`.

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

`index_` is a zero-based index into a snapshot taken at query time and sorted in ascending
lexicographic order by filter bytes, not registration order. On success, `filter_out_` contains only
the filter bytes and has no terminating NUL. This output is therefore not a C string. On entry,
`*filter_len_inout_` is the buffer size; on return, it is the filter length in bytes.
`is_pattern_out_` is an optional output and may be NULL. When provided, it reports whether the
filter is a pattern subscription. All raw subscriptions are byte-prefix filters, so it receives `0`.

If the buffer is too small, the function writes the required length to `*filter_len_inout_` and
returns `ZLINK_CONFIG_BUFFER_TOO_SMALL` with `errno == ENOBUFS`. It writes no partial data to
`filter_out_` and leaves `*is_pattern_out_` unchanged. It does not consume or modify the subscription
inventory, so the caller can retry the same `index_` with a sufficient buffer.

Applicable types: raw SUB, raw XSUB.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `ENOENT` if the index is out of range. `ENOBUFS` if the buffer is too small. `ENOTSUP` if
the handle type does not support subscription queries.

**See also:** `zlink_set_subscription`, `zlink_get_sub_option`

## 7. Implementation and contract test verification requirements

Verify the following through only the public surface: the subscription functions, SUB option set
and get, `zlink_subscribe_part` results, return values, and errno. Each item maps to one unit test.

**Subscription registration and removal**

- A filter registered through `zlink_set_subscription` matches by byte prefix—a message is received
  through `zlink_subscribe_part` when its topic begins with the filter bytes before the terminating
  NUL.
- An empty-string filter subscribes to every message.
- A filter with a trailing `*` matches `*` as a literal byte; it is not expanded as a wildcard.
- `zlink_unset_subscription` removes the subscription whose previously registered prefix matches
  the filter bytes before the terminating NUL.
- For `zlink_set_subscription` and `zlink_unset_subscription`, a NULL `handle_` produces `EFAULT`;
  a NULL `filter_` or a handle type that does not support subscription registration or removal
  produces `EINVAL`.

**Subscription inventory queries**

- Reading the read-only `ZLINK_SUB_OPT_TOPICS_COUNT` through `zlink_get_sub_option` returns the
  number of subscribed topics as an `int`.
- The zero-based `index_` for `zlink_subscription_at` follows snapshot order sorted by filter bytes,
  not registration order.
- On success, `filter_out_` contains only the filter bytes and no terminating NUL.
  `*filter_len_inout_` is that byte length, and `filter_out_` is not a C string.
- `is_pattern_out_` is an optional output and may be NULL. When provided, it receives `0` because
  every raw subscription is a byte-prefix filter.
- If the buffer is too small, the function writes the required length to `*filter_len_inout_` and
  returns `ZLINK_CONFIG_BUFFER_TOO_SMALL` with `ENOBUFS`. It writes no partial data to `filter_out_`,
  leaves `*is_pattern_out_` unchanged, and allows a retry of the same `index_` with a sufficient
  buffer.
- An out-of-range index produces `ENOENT`; a handle type that does not support subscription queries
  produces `ENOTSUP`.

**Topic-part receive**

- A successful `zlink_subscribe_part` copies the binary topic bytes into the caller's buffer without
  a NUL byte and transfers ownership of the payload part to the caller, which calls
  `zlink_msg_close(part_out_)` exactly once.
- On raw SUB and XSUB, `source_rid_out_` always receives `NULL`.
- If `topic_id_capacity_` is zero or smaller than the topic, the function writes the required length
  to `*topic_id_len_out_` and returns `ZLINK_RECV_BUFFER_TOO_SMALL` with `ENOBUFS`. The queued topic
  and payload are not consumed, so a retry with a sufficient buffer receives the same message;
  `part_out_` and every output other than `topic_id_len_out_` remain unchanged.
- If capacity is greater than zero but `topic_id_buf_` is NULL, the function returns
  `ZLINK_RECV_INVALID_HANDLE` with `EFAULT` before inspecting or consuming the queue, and every
  output and `part_out_` remains unchanged.
- For a multipart message, the next payload part follows while `*has_more_out_` is
  `ZLINK_PART_MORE`, and the last part sets it to `ZLINK_PART_FINAL`.

**Automatic HWM defaults**

- An application direction with an explicitly set `RCVHWM` is excluded from automatic distribution.
- Auto HWM does not change `RCVBUF`.

**Absence of receive flow state**

- Calling `zlink_socket_set_receive_flow_state()` on a SUB socket returns
  `ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP` and changes nothing.
- A SUB socket monitor does not set `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` and does not emit
  `ZLINK_EVENT_SEND_FLOW_PAUSED`, `ZLINK_EVENT_SEND_FLOW_RESUMED`, or
  `ZLINK_EVENT_FLOW_STATE_STALE`.

**Common return convention**

- The functions above that return `zlink_config_result_t` return `ZLINK_CONFIG_OK` on success and a
  `zlink_config_result_t` value on failure. `zlink_errno()` retains the detailed internal errno for
  diagnostics.

[Auto HWM §5](../systems/06-auto-hwm.en.md#5-implementation-and-contract-test-verification-requirements)
owns verification of Auto HWM budget calculation and admission.
