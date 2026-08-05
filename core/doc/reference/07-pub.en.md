[한국어](07-pub.ko.md) | English

[Reference index](README.en.md)

# 07. PUB

A publish-only, topic-based fan-out raw socket type. PUB is send-only — no receive function
applies. It shares `zlink_send_ready_handler` (Socket lifecycle category) with other socket
types. The exact signatures are owned by the [PUB specification](../spec/core/socket/02-pub.en.md).

---

## `zlink_set_pub_option` / `zlink_get_pub_option`

Sets or reads a PUB/XPUB-specific option.

```c
int nodrop = 1;
zlink_set_pub_option(s, ZLINK_PUB_OPT_NODROP, &nodrop, sizeof(nodrop));
```

**Parameters.** `option_` is a `zlink_pub_option_t` value: `VERBOSE`/`VERBOSER` (pass
subscribe/unsubscribe messages upstream), `MANUAL`/`MANUAL_LAST_VALUE` (manual subscription
management and last-value caching), `NODROP` (default `0` — see below), `WELCOME_MSG` (sent to
new subscribers on connect), `TOPICS_COUNT` (read-only), `APPROVE_SUBSCRIBE`/
`REJECT_SUBSCRIBE` (manual-mode subscription decisions).

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success. Use
`zlink_set_option`/`zlink_get_option` (Socket options and identity category) instead for options
shared across every socket type.

**When to use.** Fanout delivery is lossy by default: when the HWM is reached,
`zlink_publish_part` drops the message for the affected subscriber and reports success. Set
`ZLINK_PUB_OPT_NODROP` to `1` only if the application needs a full send queue to backpressure the
publisher instead of silently dropping — `zlink_publish_part` then returns
`ZLINK_SUBMIT_BACKPRESSURED` — but this couples the publisher to its slowest subscriber, since
one full pipe then stops delivery to every subscriber on the socket. Reliable delivery that must
not depend on subscriber speed belongs on a request-reply socket, not PUB/SUB.

---

## `zlink_publish_part`

Publishes one message part from a `PUB` or `XPUB` socket, addressed by topic.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_len);
memcpy(zlink_msg_data(&part), payload, payload_len);
zlink_publish_part(pub, "lobby.events", &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `topic_id_` is `NULL` to use the wire-prefix convention (the first frame carries
the topic), or a NUL-terminated string with no embedded NUL to have Core prepend those bytes as
the topic frame — there is no separate topic length limit; topic bytes count toward the
message/storage size limits. `part_`/`flags_`/`part_flag_` follow the same rules as
`zlink_send_part` (PAIR category): content is consumed on both outcomes, and
`ZLINK_PART_MORE`/`ZLINK_PART_FINAL` start/continue/end a multipart sequence on the same thread
without changing the topic or flags mid-sequence.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on success.
`ZLINK_SUBMIT_INVALID_ARGUMENT` with `EMSGSIZE` for topic bytes exceeding the size limits.
`ZLINK_SUBMIT_OUT_OF_MEMORY` with `ENOMEM` if topic-frame storage allocation fails.
`ZLINK_SUBMIT_NOT_SUPPORTED` with `ENOTSUP` for a raw socket type other than `PUB`/`XPUB`.
`ZLINK_SUBMIT_BACKPRESSURED` for a non-blocking call that can't proceed immediately.

**When to use.** As with `zlink_send_part`, Core stages a multipart publish record atomically —
any failure in the open sequence discards every staged part with nothing becoming visible to
subscribers, and a retry resubmits the whole record from its first part using retained copies.

---

See the [PUB specification](../spec/core/socket/02-pub.en.md) for the full rationale. Send-ready
notification uses `zlink_send_ready_handler` (Socket lifecycle category) — not repeated here.
