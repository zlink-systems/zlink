
<!-- zlink-nav:start -->
[← zlink overview](01-overview.en.md) | [Raw Messaging Reliability →](reliability.en.md)
<!-- zlink-nav:end -->

# Choosing a socket pattern

Choose the pattern from message direction, peer selection, and framing needs.

| Requirement | Pattern |
|---|---|
| One-to-one communication | PAIR |
| Topic distribution | PUB/SUB |
| Subscription-aware proxy | XPUB/XSUB |
| Asynchronous clients and workers | DEALER |
| Explicit peer routing | ROUTER |
| External byte-stream clients | STREAM |

## Common receive model

Raw sockets normally use part-wise receive with a poller. Register the socket
for `ZLINK_POLLIN`, wait, then call its typed receive function until the
multipart message reaches `ZLINK_PART_FINAL`.

- PAIR uses `zlink_recv_part()`.
- SUB uses `zlink_subscribe_part()` and returns the topic separately.
- XPUB uses `zlink_xpub_recv_part()` for subscription notifications.
- DEALER uses `zlink_recv_part()` for ordinary DATA; request replies arrive through
  `zlink_completion_recv()`.
- ROUTER uses `zlink_router_recv_part()` and returns the peer and an opaque reply token.
- STREAM selects RAW (`zlink_recv_part()`) or PACKET
  (`zlink_stream_recv_packet()`) before its first bind or connect.

To take a multipart record in one call instead of a per-part loop, use whole-message
receive: `zlink_recv()` for PAIR/DEALER and `zlink_router_recv()` for ROUTER fill every
part of a record into a caller-provided `zlink_msg_t` array (on insufficient capacity the
record is preserved and `ZLINK_RECV_BUFFER_TOO_SMALL` returns). The contract is owned by
[Socket Common](../spec/core/socket/README.en.md#zlink_recv-and-zlink_router_recv).

Monitor handles and generic timers can be registered with the same poller.

## Routing-id disconnect

`zlink_disconnect_rid()` requests termination of the matching peer connection.
Use it when receive metadata identifies the peer but the endpoint string is not
available.

## Detailed guides

- [PAIR](03-1-pair.en.md), [PUB/SUB](03-2-pubsub.en.md), [DEALER](03-3-dealer.en.md)
- [ROUTER](03-4-router.en.md), [STREAM](03-5-stream.en.md), [Proxy](03-6-proxy.en.md)
