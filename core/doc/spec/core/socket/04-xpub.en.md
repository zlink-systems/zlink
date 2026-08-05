[한국어](04-xpub.ko.md) | English

[Spec Index](../../README.en.md) · [Core Index](../README.en.md) · [Socket Common](README.en.md)

# Socket — XPUB

Extended publisher with subscription forwarding and manual control. XPUB
receives subscription events from subscribers and supports manual
subscription management.

## Pub Options (`zlink_pub_option_t`)

Used with `zlink_set_pub_option()` / `zlink_get_pub_option()`.

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

| Constant | Description |
|---|---|
| `ZLINK_PUB_OPT_VERBOSE` | Pass all subscription messages upstream (`int`; 0 or 1) |
| `ZLINK_PUB_OPT_VERBOSER` | Pass all subscribe and unsubscribe messages upstream (`int`; 0 or 1) |
| `ZLINK_PUB_OPT_MANUAL` | Enable manual subscription management (`int`; 0 or 1) |
| `ZLINK_PUB_OPT_MANUAL_LAST_VALUE` | Enable last-value caching in manual mode (`int`; 0 or 1) |
| `ZLINK_PUB_OPT_NODROP` | Do not silently drop messages on HWM; return `EAGAIN` instead (`int`; 0 or 1, default `0`) |
| `ZLINK_PUB_OPT_WELCOME_MSG` | Message sent to new subscribers on connect (`binary`) |
| `ZLINK_PUB_OPT_TOPICS_COUNT` | Number of subscribed topics (get-only, `int`) |
| `ZLINK_PUB_OPT_APPROVE_SUBSCRIBE` | Approve a pending subscription in manual mode (`binary`) |
| `ZLINK_PUB_OPT_REJECT_SUBSCRIBE` | Reject a pending subscription in manual mode (`binary`) |

`ZLINK_PUB_OPT_NODROP` defaults to `0`. Fanout delivery is lossy: when the HWM
is reached, `zlink_publish_part()` drops the message for the affected
subscriber and reports success. Callers that need a full send queue to
backpressure the publisher instead of dropping must set this option explicitly
to `1`; `zlink_publish_part()` then returns `ZLINK_SUBMIT_BACKPRESSURED`.

Setting `1` couples the publisher to its slowest subscriber, because a single
full pipe stops delivery to every subscriber on the socket. Reliable delivery
that must not depend on subscriber speed belongs on a request-reply socket, not
on XPUB/XSUB.

## Functions

### zlink_set_pub_option

Set a pub-specific option.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_pub_option (void *handle_,
                           zlink_pub_option_t option_,
                           const void *optval_,
                           size_t optvallen_);
```

Configures a PUB/XPUB socket option. Use `zlink_set_option()` for common
options shared across all socket types.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_get_pub_option`, `zlink_set_option`

---

### zlink_get_pub_option

Get a pub-specific option.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_pub_option (void *handle_,
                           zlink_pub_option_t option_,
                           void *optval_,
                           size_t *optvallen_);
```

Retrieves the current value of a PUB/XPUB socket option.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_set_pub_option`

---

### zlink_publish_part

Publish one message part from a raw `XPUB` socket.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_publish_part (
  void *subject_,
  const char *topic_id_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

When `topic_id_ == NULL`, the first message frame carries the topic according
to the wire-prefix convention. Otherwise `topic_id_` must be a NUL-terminated
byte string with no embedded NUL. Every byte before the terminating NUL is the
topic, and Core prepends those bytes as the topic frame. There is no separate
topic-specific maximum; topic bytes count toward the message and storage size
limits. Exceeding those limits returns `ZLINK_SUBMIT_INVALID_ARGUMENT` with
`EMSGSIZE`. Failure to allocate
topic-frame storage returns `ZLINK_SUBMIT_OUT_OF_MEMORY` with `ENOMEM`. A
multipart message started with `ZLINK_PART_MORE` continues on the same thread
with the same topic and flags through `ZLINK_PART_FINAL`.

This function consumes the content of `part_` on both success and failure. If
the same content may be needed again, copy it before the call and initialize a
consumed `zlink_msg_t` before reuse. Pass `ZLINK_DONTWAIT` in `flags_` for
non-blocking publish; a call that cannot proceed immediately returns
`ZLINK_SUBMIT_BACKPRESSURED`.

Core stages successful intermediate parts as one publish record until
`ZLINK_PART_FINAL` succeeds. If an intermediate or final submit in the open
sequence fails, Core atomically discards the previously staged parts and the
failed part and closes the sequence. No part of that record becomes visible to
subscribers. The failed call still consumes `part_`, and the next publish
starts the first part of a new record. After any failure, including
backpressure, a retry resubmits the retained entire record from its first part.

Applicable types are raw `PUB` and raw `XPUB`. Other types return
`ZLINK_SUBMIT_NOT_SUPPORTED` with `errno == ENOTSUP`. See the
[errno map](../04-errno-map.en.md) for the full result mapping.

**See also:** `zlink_xpub_recv_part`, `zlink_send_ready_handler`

---

### zlink_xpub_recv_part

Receive a subscription event from an XPUB socket.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_xpub_recv_part (void *xpub_,
                               const zlink_routing_id_t **source_rid_out_,
                               int *subscribed_out_,
                               char *topic_id_buf_,
                               size_t topic_id_capacity_,
                               size_t *topic_id_len_out_,
                               zlink_recv_flags_t flags_);
```

Receives the next subscription event in recv mode. On success,
`*source_rid_out_` is set to the library-owned routing ID of the
subscribing peer (valid until the next call on this socket),
`*subscribed_out_` is 1 for subscribe or 0 for unsubscribe, and
`topic_id_buf_` / `*topic_id_len_out_` receive the topic bytes
(binary-safe). The caller supplies the buffer capacity via
`topic_id_capacity_`; if the topic is longer than the capacity the call fails
with `errno = EMSGSIZE`.

Applicable types: raw XPUB only.

**Returns:** `ZLINK_RECV_OK` on success; otherwise a `zlink_recv_result_t`
value. Detail errnos other than `EAGAIN`/`ETERM` (for example `EMSGSIZE` for an
over-capacity topic, or `EINVAL` for a non-XPUB subject) surface as
`ZLINK_RECV_INTERNAL_ERROR`; `zlink_errno()` retains the detailed internal errno
for diagnostics.

**Errors:** `EFAULT` if `xpub_` is NULL. `EAGAIN` if `ZLINK_DONTWAIT`
was set and no event is available. `EMSGSIZE` if the topic is longer than
`topic_id_capacity_`. `EINVAL` if the subject is not XPUB.

**See also:** `zlink_publish_part`

---

### zlink_send_ready_handler

Install or replace the send-ready callback.

```c
ZLINK_EXPORT zlink_handler_result_t zlink_send_ready_handler (
  void *s_, zlink_send_ready_handler_fn handler_, void *userdata_);
```

The handler is replace-only. Passing NULL is invalid. A successful replace is
visible from the next writable transition. If called reentrantly from the
same handle's send-ready callback, the call fails with `errno=EDEADLK`.

Supported subjects: raw `PAIR`, `PUB`, `XPUB`, `DEALER`, `ROUTER`, and
`STREAM`. Send-ready is independent from receive mode. This
callback and `ZLINK_POLLOUT` expose the same send-recovery readiness axis: a
readiness signal means it is worth retrying send, not that the retry is
guaranteed to succeed. Unsupported subjects return `ENOTSUP`.

**Returns:** `ZLINK_HANDLER_OK` on success; otherwise a `zlink_handler_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_publish_part`
