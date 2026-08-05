[한국어](02-pub.ko.md) | English

[Spec Index](../../README.en.md) · [Core Index](../README.en.md) · [Socket Common](README.en.md)

# Socket — PUB

Publish-only socket, topic-based fan-out. PUB is send-only; no recv
functions apply.

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
on PUB/SUB.

## Automatic HWM defaults

PUB is classified as the `fanout` policy class by the context automatic HWM
policy. The active auto-HWM profile selects the unit budget and message-size
cap; the default profile is `balanced`. Manual `SNDHWM` or `SNDBUF` settings
override the automatic values.

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

Publish one message part from a raw `PUB` or `XPUB` socket.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_publish_part (void *subject_,
                                                       const char *topic_id_,
                                                       zlink_msg_t *part_,
                                                       zlink_send_flags_t flags_,
                                                       zlink_part_flag_t part_flag_);
```

When `topic_id_ == NULL`, the first message frame carries the topic according
to the wire-prefix convention. When it is non-NULL, `topic_id_` must be a
NUL-terminated byte string with no embedded NUL. Every byte before the
terminating NUL is the topic, and Core prepends those bytes as the topic frame.
There is no separate topic-specific maximum; topic bytes count toward the
message and storage size limits.
Exceeding those limits returns `ZLINK_SUBMIT_INVALID_ARGUMENT` with `EMSGSIZE`.
Failure to allocate topic-frame storage returns `ZLINK_SUBMIT_OUT_OF_MEMORY`
with `ENOMEM`. A multipart message started with `ZLINK_PART_MORE` must continue
with this function on the same thread through `ZLINK_PART_FINAL`. Do not call a
different send helper or change the topic or flags within that sequence.

This function consumes the content of `part_` on both success and failure.
Regardless of the result, the caller must make a separate copy before the call
if the same content may need to be sent again, and must initialize a consumed
`zlink_msg_t` before reusing it.

Core stages successful intermediate parts as one publish record until
`ZLINK_PART_FINAL` succeeds. If an intermediate or final submit in the open
sequence fails, Core atomically discards the previously staged parts and the
failed part and closes the sequence. No part of that record becomes visible to
subscribers. The failed call still consumes `part_`, and the next publish
starts the first part of a new record. After any failure, including
backpressure, a retry resubmits the retained entire record from its first part.

Applicable types are raw `PUB` and raw `XPUB`. Other raw socket types return
`ZLINK_SUBMIT_NOT_SUPPORTED` and set `errno` to `ENOTSUP`.

Pass `ZLINK_DONTWAIT` in `flags_` for non-blocking publish. A call that cannot
proceed immediately returns `ZLINK_SUBMIT_BACKPRESSURED`. The function still
consumes `part_` regardless of the result. See the
[errno map](../04-errno-map.en.md) for the full result mapping.

**See also:** `zlink_send_ready_handler`

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
