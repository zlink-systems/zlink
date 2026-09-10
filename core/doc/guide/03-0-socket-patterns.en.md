
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

Raw sockets normally use whole-message receive with a poller. Register the
socket for `ZLINK_POLLIN`, wait, then call the socket-specific receive function
once to receive the complete record.

- PAIR uses `zlink_recv()`.
- SUB uses `zlink_subscribe()` and returns the topic separately.
- XPUB uses `zlink_xpub_recv()` for subscription notifications.
- DEALER uses `zlink_recv()` for ordinary DATA; request replies arrive through
  `zlink_completion_recv()`.
- ROUTER uses `zlink_router_recv()` and returns the peer and an opaque reply token.
- STREAM selects RAW (`zlink_recv()`) or PACKET
  (`zlink_stream_recv_packet()`) before its first bind or connect.

`zlink_recv()` for PAIR/DEALER, `zlink_router_recv()` for ROUTER, and
`zlink_subscribe()` for SUB/XSUB fill every part of a record into a caller-provided
`zlink_msg_t` array. If capacity is insufficient, the record is preserved and
`ZLINK_RECV_BUFFER_TOO_SMALL` returns. The contract is owned by
[Socket Common](../spec/core/socket/README.en.md#zlink_recv-and-zlink_router_recv).

Monitor handles and generic timers can be registered with the same poller.

## Routing-id disconnect

`zlink_disconnect_rid()` requests termination of the matching peer connection.
Use it when receive metadata identifies the peer but the endpoint string is not
available.

## Detailed guides

- [PAIR](03-1-pair.en.md), [PUB/SUB](03-2-pubsub.en.md), [DEALER](03-3-dealer.en.md)
- [ROUTER](03-4-router.en.md), [STREAM](03-5-stream.en.md), [Proxy](03-6-proxy.en.md)
