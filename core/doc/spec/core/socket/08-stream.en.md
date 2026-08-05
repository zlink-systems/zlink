[한국어](08-stream.ko.md) | English

[Specification Index](../../README.en.md) · [Core Index](../README.en.md) · [Socket Common](README.en.md) · [Errno Map](../04-errno-map.en.md)

# Socket — STREAM

This document defines the generic raw STREAM public contract for ZLink Core
raw STREAM. It is for C API and bindings developers that exchange byte records or
fixed-framing packets over routed TCP or WebSocket connections.

## 1. Scope

STREAM is a bind-only raw socket that assigns a 4-byte routing ID to every
accepted client connection. It does not support `zlink_connect()`. An
application addresses a client by routing ID when sending and reads the source
routing ID from receive results.

STREAM does not interpret application payloads or higher-level protocol semantics.

## 2. Creation, bind, and options

```c
ZLINK_EXPORT void *zlink_socket (void *context_, zlink_socket_type_t type_);
ZLINK_EXPORT zlink_bind_result_t zlink_bind (void *s_, const char *addr_);
ZLINK_EXPORT zlink_close_result_t zlink_close (void *s_);

typedef enum zlink_stream_option_t
{
    ZLINK_STREAM_OPT_NOTIFY = 0x3501
} zlink_stream_option_t;

ZLINK_EXPORT zlink_config_result_t zlink_set_stream_option (
  void *handle_, zlink_stream_option_t option_,
  const void *optval_, size_t optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_get_stream_option (
  void *handle_, zlink_stream_option_t option_,
  void *optval_, size_t *optvallen_);
```

Create the socket with `zlink_socket(context_, ZLINK_SOCKET_STREAM)`.
`ZLINK_STREAM_OPT_NOTIFY` is an `int` with value 0 or 1 and is set before
bind. A value of 1 exposes client connect and disconnect notifications as
zero-length data records. The source routing ID identifies the client.

Common HWM, timeout, linger, TLS, and buffer options use
`zlink_set_option()` and `zlink_get_option()`.

## 3. Receive modes

One STREAM handle uses exactly one of these modes:

1. raw part receive: `zlink_recv_part()` receives parts of raw records;
2. raw callback: `zlink_recv_handler()` delivers raw records to a callback;
3. packet callback: `zlink_stream_packet_handler()` assembles and delivers
   fixed-framing packets.

The first raw part receive or handler registration fixes the receive mode.
Activating another receive mode or registering another handler on the same
handle fails with a busy result and `errno == EBUSY`. Data-plane
`ZLINK_POLLIN` belongs to raw part receive mode. A send-ready handler and
`ZLINK_POLLOUT` are independent of receive mode.

## 4. Routed part send

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid (
  void *s_,
  const zlink_routing_id_t *target_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

STREAM sends one raw data part at a time to a target client. `target_rid_`
must be a valid 4-byte routing ID assigned by this STREAM socket, and
`part_flag_` must be `ZLINK_PART_FINAL`. `ZLINK_PART_MORE` returns
`ZLINK_SUBMIT_NOT_SUPPORTED` with `errno == ENOTSUP`.

STREAM send never opens a multipart sequence. After a `ZLINK_PART_MORE`
failure, no part is staged and the next call is an independent single-part
record. The atomic multipart-abort rule of other raw sockets therefore does
not apply to STREAM.

Success consumes the content of `part_`. When backpressure returns
`ZLINK_SUBMIT_BACKPRESSURED` with `errno == EAGAIN`, the content remains owned
by the caller and may be retried. Other failures consume the content. Keeping
a payload copy before the call gives the caller one uniform recovery strategy
across all failure results.

A missing connection returns `ZLINK_SUBMIT_NOT_CONNECTED`. See the
[Errno Map](../04-errno-map.en.md) for the full result mapping.

## 5. Raw part receive

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (
  void *s_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

`part_out_` must be an initialized message and is required together with
`has_more_out_`. `source_rid_out_` is optional. On success it receives a
Core-owned borrowed view of the source client's routing ID. Copy the view
before the next raw receive if it must remain valid.

On success, ownership of the received part transfers to the caller, which
must call `zlink_msg_close(part_out_)` exactly once. A failure before a part is
received does not transfer ownership. `*has_more_out_` is
`ZLINK_PART_MORE` when another part follows and `ZLINK_PART_FINAL` for the
last part. A `ZLINK_DONTWAIT` call with no data returns
`ZLINK_RECV_NO_DATA`.

## 6. Raw callback

```c
ZLINK_EXPORT zlink_handler_result_t zlink_recv_handler (
  void *s_, zlink_socket_msg_handler_fn handler_, void *userdata_);
```

Raw callback mode is supported only by STREAM. The source routing ID is a
borrowed view valid only for the callback. Ownership of every delivered
message part transfers to the callback, which must consume or close each part
exactly once. Registering a receive handler again or closing the same handle
from inside the callback fails with a busy result and `errno == EBUSY`.

## 7. Packet callback and framing

```c
typedef void (*zlink_stream_packet_handler_fn) (
  void *stream_,
  const zlink_routing_id_t *source_rid_,
  zlink_msg_t *header_,
  zlink_msg_t *body_,
  void *userdata_);

ZLINK_EXPORT zlink_handler_result_t zlink_stream_packet_handler (
  void *stream_, zlink_stream_packet_handler_fn handler_, void *userdata_);
```

Packet mode assembles this frame in order on each client byte stream:

```text
+----------------+----------------+----------------+---------------+
| header_size:u16| body_size:u32  | header bytes   | body bytes    |
+----------------+----------------+----------------+---------------+
| big endian     | big endian     | exact length   | exact length  |
+----------------+----------------+----------------+---------------+
```

`header_size` is an unsigned 16-bit length and `body_size` is an unsigned
32-bit length. Either payload may have length zero; the callback still
receives a valid zero-length `zlink_msg_t`, not `NULL`. The source routing ID
is a borrowed view valid only for the callback. Ownership of `header_` and
`body_` transfers to the callback, which must consume or close each message
exactly once.

## 8. Send readiness and thread safety

```c
ZLINK_EXPORT zlink_handler_result_t zlink_send_ready_handler (
  void *s_, zlink_send_ready_handler_fn handler_, void *userdata_);
```

Send readiness means that retrying a previously backpressured submit is
worthwhile; it does not guarantee success of the next submit. A handler can
be replaced but cannot be removed with `NULL`. Reentrant registration from
the same send-ready callback returns `ZLINK_HANDLER_DEADLOCK` with
`errno == EDEADLK`.

The [Socket Common](README.en.md) contract defines public socket-handle thread
safety and close behavior. The same `zlink_msg_t` cannot be used concurrently
from multiple threads.
