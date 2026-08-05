[한국어](09-xpub.ko.md) | English

[Reference index](README.en.md)

# 09. XPUB

An extended publisher that also receives subscription events from subscribers, with manual
subscription control. XPUB shares `zlink_set_pub_option`/`zlink_get_pub_option` and
`zlink_publish_part` with PUB (PUB category) unchanged — this category documents only the one
entry point unique to XPUB: receiving the subscription events themselves. The exact signatures
are owned by the [XPUB specification](../spec/core/socket/04-xpub.en.md).

---

## `zlink_xpub_recv_part`

Receives the next subscribe/unsubscribe event from an XPUB socket's subscribers.

```c
const zlink_routing_id_t *subscriber_rid;
int subscribed;
char topic[256];
size_t topic_len;
zlink_xpub_recv_part(xpub, &subscriber_rid, &subscribed, topic, sizeof(topic), &topic_len, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `source_rid_out_` receives a library-owned view of the subscribing peer's routing
ID, valid until the next call on this socket. `subscribed_out_` is set to `1` for subscribe or
`0` for unsubscribe. `topic_id_buf_`/`topic_id_capacity_` is the caller's buffer;
`topic_id_len_out_` receives the topic's actual length.

**Return and errno.** Returns `zlink_recv_result_t` — `ZLINK_RECV_OK` on success. `EFAULT` if the
socket handle is `NULL`. `EAGAIN` with `ZLINK_DONTWAIT` and no event available. `EMSGSIZE` if the
topic exceeds `topic_id_capacity_`. `EINVAL` if the subject isn't XPUB. Detail errnos other than
`EAGAIN`/`ETERM` surface through the return as `ZLINK_RECV_INTERNAL_ERROR`, with the specific
cause available via `zlink_errno()`.

**When to use.** This is a recv-mode call specific to XPUB — raw XPUB is the only applicable
type. Use `ZLINK_PUB_OPT_VERBOSE`/`ZLINK_PUB_OPT_VERBOSER` (PUB category) to control whether
duplicate subscribe/unsubscribe events are forwarded, and `ZLINK_PUB_OPT_MANUAL` if the
application wants to approve or reject subscriptions itself via
`ZLINK_PUB_OPT_APPROVE_SUBSCRIBE`/`ZLINK_PUB_OPT_REJECT_SUBSCRIBE` instead of Core admitting
them automatically.

---

See the [XPUB specification](../spec/core/socket/04-xpub.en.md) for the full rationale. Publish
and options use `zlink_publish_part`/`zlink_set_pub_option`/`zlink_get_pub_option` (PUB
category); send-ready notification uses `zlink_send_ready_handler` (Socket lifecycle category) —
none of those are repeated here.
