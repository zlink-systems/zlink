[한국어](03-4-router.ko.md)

# ROUTER

ROUTER receives a routing id with each inbound message and uses that id to
select a peer for outbound messages. Use it when one socket must communicate
with multiple DEALER or ROUTER peers.

## Receive a message

`zlink_router_recv_part()` returns one payload part at a time. The routing-id
view remains valid until the next receive-like call on the same thread. Copy it
when it must outlive that call.

```c
const zlink_routing_id_t *source_rid = NULL;
uint64_t request_seq = 0;
zlink_msg_t part;
zlink_part_flag_t more;

zlink_msg_init(&part);
zlink_recv_result_t rc = zlink_router_recv_part(
    router, &source_rid, &request_seq, &part, &more, 0);
/* source_rid selects the peer; more describes multipart continuation. */
```

For ordinary routed traffic, `request_seq` is zero. A positive sequence
identifies a request that can be answered with `zlink_router_reply_part()`.

## Send routed data

Use `zlink_send_part_rid()` and pass the peer routing id. Every part except the
last uses `ZLINK_PART_MORE`; the last uses `ZLINK_PART_FINAL`.

## Request and reply

`zlink_router_request_part()` submits a routed request and completes through a
reply callback. A received request is answered with
`zlink_router_reply_part()`, using the source routing id and request sequence
returned by the receive call.

ROUTER mandatory and handover behavior is configured with the typed router
options described in [Socket Options](12-socket-options.en.md).

See [Routing IDs](08-routing-id.en.md) for lifetime and copy rules and
[Thread Safety](11-thread-safety.en.md) for same-handle concurrency.
