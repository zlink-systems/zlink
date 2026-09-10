
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

Core uses part-wise multipart APIs:

- `zlink_send_part()` sends ordinary raw traffic.
- `zlink_send_part_rid()` selects a routed peer.
- `zlink_publish_part()` publishes a topic and payload.
- Typed receive functions return one part and a `ZLINK_PART_MORE` or
  `ZLINK_PART_FINAL` flag.
- DEALER and ROUTER requests return nonzero completion IDs; receive their replies
  and terminal results with `zlink_completion_recv()` and release each record with
  `zlink_completion_close()`.

Receive comes in two shapes.

- **Per-part** `*_recv_part()` — returns one part and `has_more` per call. Use it for
  single-part records, for streaming parts one at a time (partial consumption), and
  for the lowest-allocation path.
- **Whole-message** `zlink_recv()` (PAIR/DEALER) and `zlink_router_recv()` (ROUTER) —
  fill every part of a record into a caller-provided `zlink_msg_t` array in a single
  call, cutting call and boundary counts on mostly-multipart workloads. If the array
  capacity is smaller than the record's part count, the record is not consumed and
  `ZLINK_RECV_BUFFER_TOO_SMALL` (`ENOBUFS`) returns the needed count, so retrying with
  a larger array receives the same record exactly once. Close the filled array at once
  with [`zlink_multipart_close()`](../spec/core/02-message.en.md#zlink_multipart_close).

Which to use: whole-message `recv` for mostly-multipart records (fewer calls and
allocations), `recv_part` for single-part, streaming, or lowest-allocation needs. The
two coexist on the same socket and share the same contract (single-consumer, record
atomicity).

A message part passed to a `*_part` send is consumed by Core whether the call
succeeds or fails — afterwards the part is left in the empty initialized state,
so copy it before the call if you may need to send it again. A received part
must be closed or moved exactly once.

## Eventing

Pollers wait for socket, file-descriptor, and generic-timer readiness. Socket
monitors report raw transport and protocol events and expose a current status
snapshot. Generic timers are consumed by receive, directly or after poller readiness.

The public header comments and [Core specification](../spec/core/README.en.md) are
the source of truth for result values, ownership, and concurrency.
