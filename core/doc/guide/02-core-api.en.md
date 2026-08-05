[한국어](02-core-api.ko.md)

# Core C API

Include `<zlink.h>`. Create a context with `zlink_ctx_new()`, create typed raw
sockets with `zlink_socket()`, and terminate the context after all sockets are
closed.

## Socket lifecycle

Use `zlink_bind()` and `zlink_connect()` to establish endpoints.
`zlink_unbind()` and `zlink_disconnect()` remove them. `zlink_close()` releases
the socket. A close attempt may report busy while another admitted operation or
callback is active.

## Configuration

`zlink_set_option()` and `zlink_get_option()` handle common options. Router,
dealer, stream, pub, and sub families have typed option functions. Configure a
routing id or TLS before the connection handshake needs it.

## Message I/O

Core uses part-wise multipart APIs:

- `zlink_send_part()` sends ordinary raw traffic.
- `zlink_send_part_rid()` selects a routed peer.
- `zlink_publish_part()` publishes a topic and payload.
- Typed receive functions return one part and a `ZLINK_PART_MORE` or
  `ZLINK_PART_FINAL` flag.
- DEALER and ROUTER request functions complete through `zlink_reply_handler_fn`.

The caller owns a message part until a successful send consumes it. A received
part must be closed or moved exactly once.

## Eventing

Pollers wait for socket, file-descriptor, and generic-timer readiness. Socket
monitors report raw transport and protocol events and expose a current status
snapshot. Generic timers can be consumed by receive, callback, or poller.

The public header comments and [Core specification](../spec/core/README.en.md) are
the source of truth for result values, ownership, and concurrency.
