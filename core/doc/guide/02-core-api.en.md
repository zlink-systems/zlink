
<!-- zlink-nav:start -->
[← Core performance](10-performance.en.md) | [Socket options →](12-socket-options.en.md)
<!-- zlink-nav:end -->

# Core C API

Include `<zlink.h>`. Create a context with `zlink_ctx_new()`, create typed raw
sockets with `zlink_socket()`, and terminate the context after all sockets are
closed.

## Socket lifecycle

Use `zlink_bind()` and `zlink_connect()` to establish endpoints.
`zlink_unbind()` and `zlink_disconnect()` remove them. `zlink_close()` releases
the socket. A close attempt may report busy while another admitted operation is
active.

## Configuration

`zlink_set_option()` and `zlink_get_option()` handle common options. Router,
dealer, stream, pub, and sub families have typed option functions. Configure a
routing id or TLS before the connection handshake needs it.

## Message I/O

Core sends and receives one complete message record per call. Array order and
part count define the multipart boundary.

- `zlink_send()` sends every part of ordinary raw traffic as an array.
- `zlink_send_rid()` selects a routed peer and sends every part as an array.
- `zlink_publish()` publishes a topic and an array of payload parts.
- `zlink_recv()`, `zlink_router_recv()`, and `zlink_subscribe()` fill a
  caller-provided array with the complete record and return its part count.
  `zlink_xpub_recv()` receives subscription events.
- DEALER and ROUTER requests return nonzero completion IDs; receive their replies
  and terminal results with `zlink_completion_recv()` and release each record with
  `zlink_completion_close()`.

If the receive array is smaller than the record's part count, the record is not
consumed and `ZLINK_RECV_BUFFER_TOO_SMALL` (`ENOBUFS`) returns the needed count.
Grow the array and retry to receive the same record exactly once. Close the filled
array with [`zlink_multipart_close()`](../spec/core/02-message.en.md#zlink_multipart_close).

Core consumes every message part in a send array on both success and failure.
Each slot is left empty and initialized, so retain a copy of the complete record
before the call if it may need to be sent again.

## Eventing

Pollers wait for socket, file-descriptor, and generic-timer readiness. Socket
monitors report raw transport and protocol events and expose a current status
snapshot. Generic timers are consumed by receive, directly or after poller readiness.

The public header comments and [Core specification](../spec/core/README.en.md) are
the source of truth for result values, ownership, and concurrency.
