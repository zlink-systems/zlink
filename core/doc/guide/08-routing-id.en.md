[한국어](08-routing-id.ko.md) | [Message API →](09-message-api.en.md)

# Routing IDs

A routing id identifies a connected peer on routed raw sockets. Core exposes it
as `zlink_routing_id_t`, which contains a byte sequence and its length.

## Configure a local id

Call `zlink_set_routing_id()` before connecting or binding when an application
needs a stable local identity. Read the configured value with
`zlink_get_routing_id()`.

```c
const char id[] = "worker-7";
zlink_set_routing_id(socket, id, sizeof(id) - 1);
/* Configure the id before the connection handshake uses it. */
```

## Receive and reply

`zlink_recv_part()`, `zlink_subscribe_part()`, and
`zlink_router_recv_part()` return a pointer to a thread-local routing-id view.
The next receive-like call on the same thread may invalidate it.

```c
const zlink_routing_id_t *source_rid = NULL;
uint64_t request_seq = 0;
zlink_msg_t part;
zlink_part_flag_t more;

zlink_msg_init(&part);
zlink_router_recv_part(
    router, &source_rid, &request_seq, &part, &more, 0);
/* Copy source_rid now if another receive may run before it is used. */
```

Use a received routing id with `zlink_send_part_rid()` for ordinary routed
traffic. For a request, combine it with the returned sequence and call
`zlink_router_reply_part()`.

## Disconnect a peer

`zlink_disconnect_rid()` requests asynchronous termination of the matching
peer connection. Success means the termination request was accepted; it does
not make transport shutdown synchronous.
