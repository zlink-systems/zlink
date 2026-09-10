[Message API →](09-message-api.en.md)

<!-- zlink-nav:start -->
[← Thread safety](11-thread-safety.en.md) | [PAIR Socket →](03-1-pair.en.md)
<!-- zlink-nav:end -->

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

`zlink_recv()`, `zlink_subscribe()`, and `zlink_router_recv()` return a pointer
to a socket-owned routing-id view. Each function fills a caller-provided array
with the complete payload record. The next data-receive entry on that same
socket, successful or not, invalidates the view.

```c
const zlink_routing_id_t *source_rid = NULL;
zlink_reply_token_t reply_token = 0;
zlink_msg_t parts[8];
size_t part_count = 0;

if (zlink_router_recv(router, &source_rid, &reply_token,
                     parts, 8, &part_count, ZLINK_RECV_FLAGS_NONE) == ZLINK_RECV_OK) {
    /* Copy source_rid now if another data receive on router may run first. */
    zlink_multipart_close(parts, part_count);
}
```

Use a received routing id with `zlink_send_rid()` for ordinary routed traffic.
For a request, pass the returned opaque token and an array of reply parts to
`zlink_reply()`; ordinary DATA has token `0`.

## Disconnect a peer

`zlink_disconnect_rid()` requests asynchronous termination of the matching
peer connection. Success means the termination request was accepted; it does
not make transport shutdown synchronous.
