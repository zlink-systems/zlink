[한국어](08-sub.ko.md) | English

[Reference index](README.en.md)

# 08. SUB

A subscribe raw socket type with topic filtering. SUB is receive-only for data; subscription
management is its control plane. It shares `zlink_set_subscription`/`zlink_unset_subscription`/
`zlink_subscription_at` with XSUB (XSUB category cross-links here rather than duplicating). The
exact signatures are owned by the [SUB specification](../spec/core/socket/03-sub.en.md).

---

## `zlink_set_sub_option` / `zlink_get_sub_option`

Sets or reads a SUB/XSUB-specific option.

```c
int topics;
size_t len = sizeof(topics);
zlink_get_sub_option(s, ZLINK_SUB_OPT_TOPICS_COUNT, &topics, &len);
```

**Parameters.** `option_` is `ZLINK_SUB_OPT_TOPICS_COUNT` — the only SUB-specific option, and
read-only.

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success. Use
`zlink_set_option`/`zlink_get_option` (Socket options and identity category) for options shared
across every socket type.

**When to use.** Use this to inspect how many topic filters are currently registered on the
socket.

---

## `zlink_set_subscription` / `zlink_unset_subscription`

Adds or removes a byte-prefix topic filter on a raw SUB or XSUB socket.

```c
zlink_set_subscription(s, "game.scores.");
// ... later ...
zlink_unset_subscription(s, "game.scores.");
```

**Parameters.** `filter_` is a NUL-terminated string with no embedded NUL. The bytes before the
terminator are a byte-prefix filter: a message matches when its topic starts with those bytes.
An empty string subscribes to everything. There is no wildcard syntax — a trailing `*` matches
literally.

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
`EFAULT` if `handle_` is `NULL`. `EINVAL` if `filter_` is `NULL`, or the handle type doesn't
support subscribe/unsubscribe.

**When to use.** Applicable to raw SUB and raw XSUB. `unset_subscription`'s filter must match a
previously registered prefix exactly (same byte-prefix interpretation as `set_subscription`).

---

## `zlink_subscribe_part`

Receives one payload part of a topic-bearing message from a raw SUB or XSUB socket.

```c
char topic[256];
size_t topic_len;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_subscribe_part(s, NULL, topic, sizeof(topic), &topic_len, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `topic_id_buf_`/`topic_id_capacity_` is the caller's topic buffer and its size;
`topic_id_len_out_` (required) receives the actual topic length. `part_out_` (required,
already-initialized) and `has_more_out_` (required) follow the same contract as
`zlink_recv_part` (Raw receive category). `source_rid_out_` is optional and always `NULL` for raw
SUB/XSUB.

**Return and errno.** Returns `zlink_recv_result_t` — `ZLINK_RECV_OK` on success: the topic bytes
are copied into the caller's buffer with no trailing NUL, and payload-part ownership transfers to
the caller (close it exactly once, Message category). If the topic buffer is too small (including
capacity `0`), returns `ZLINK_RECV_BUFFER_TOO_SMALL` with `ENOBUFS`, writes only the required
length to `topic_id_len_out_`, and consumes nothing — retry with a larger buffer. A positive
capacity with a `NULL` buffer fails before touching the queue, returning
`ZLINK_RECV_INVALID_HANDLE` with `EFAULT`.

**When to use.** Receive every payload part of one multipart message, first through last, with
this function on the same thread — `has_more_out_` distinguishes `ZLINK_PART_MORE` from
`ZLINK_PART_FINAL` exactly as in `zlink_recv_part`.

---

## `zlink_subscription_at`

Retrieves the registered subscription filter at a given index.

```c
char filter[256];
size_t filter_len = sizeof(filter);
int is_pattern;
zlink_subscription_at(s, 0, filter, &filter_len, &is_pattern);
```

**Parameters.** `index_` is 0-based. `filter_out_`/`filter_len_inout_` is the caller's buffer —
on entry its capacity, on return the actual length written. `is_pattern_out_` always reports `0`,
since every raw subscription is a byte-prefix filter, not a pattern.

**Return and errno.** Returns `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success writes no
partial data on failure. `ENOENT` if `index_` is out of range. `ENOBUFS` if the buffer is too
small (the required length is written to `filter_len_inout_`, `is_pattern_out_` left unchanged,
and the inventory left untouched — retry the same index with a bigger buffer). `ENOTSUP` for a
handle type that doesn't support subscription query.

**When to use.** Use this to enumerate a socket's current subscriptions, e.g. for diagnostics.

---

See the [SUB specification](../spec/core/socket/03-sub.en.md) for the full rationale.
