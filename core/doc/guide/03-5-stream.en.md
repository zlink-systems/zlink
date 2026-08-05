[English](03-5-stream.en.md) | [한국어](03-5-stream.ko.md)

<!-- zlink-nav:start -->
[← ROUTER](03-4-router.en.md) | [Proxy →](03-6-proxy.en.md)
<!-- zlink-nav:end -->

# STREAM Socket

## 1. Overview

STREAM is a **server-only** socket for communicating with **external raw clients**.

Core rules:
- `ZLINK_SOCKET_STREAM` supports `zlink_bind()` only.
- Calling `zlink_connect()` on `ZLINK_SOCKET_STREAM` returns `EOPNOTSUPP`.
- Clients must use OS/Asio/WebSocket raw client stacks, not zlink STREAM sockets.
- RAW mode has no zlink-level wire framing — it is a transparent byte
  stream (the encoder/decoder pass bytes through unchanged). For
  length-delimited packets, use the packet handler, which frames as
  2-byte BE header size + 4-byte BE body size + header + body.
- At the zlink API level: raw `zlink_recv()` exposes the 4-byte
  `routing_id` then the payload frame, while the raw/packet callbacks
  pass `source_rid` as a separate callback argument.

Valid combination:

```
external raw client  <---- RAW byte stream (no framing) ---->  STREAM(server)
```

> STREAM is not directly compatible with zlink internal sockets (PAIR/PUB/SUB/DEALER/ROUTER).

---

## 2. Server Create/Bind

```c
void *stream = zlink_socket(ctx, ZLINK_SOCKET_STREAM);
int linger = 0;
zlink_set_option(stream, ZLINK_OPT_LINGER, &linger, sizeof(linger));
zlink_bind(stream, "tcp://0.0.0.0:8080");
```

Supported server transports:
- `tcp://`
- `tls://`
- `ws://`
- `wss://`

---

## 3. STREAM-Specific Behavior

STREAM is the only exception type in the raw socket family. Exactly one of
three receive models may be active on a given handle.

- **raw recv**: `zlink_recv()` pulls transport fragments directly. Pair it
  with a poller watching `ZLINK_POLLIN`.
- **raw callback**: `zlink_recv_handler()` delivers raw fragments through
  a callback. Useful for event-driven servers.
- **packet callback**: `zlink_stream_packet_handler()` delivers packets
  assembled from a fixed framing convention (2B header size + 4B body
  size + header + body, all big-endian) as header/body pairs.

The three models are mutually exclusive; a second attempt to activate a
different mode on the same handle fails with `EBUSY`. Applications pick
whichever model fits best.

STREAM-specific behavior:

- `source_rid` is auto-assigned per connection by the server,
  always fixed 4 bytes (`uint32`, big-endian).
- To close one client, pass the `source_rid` received from callback or recv
  to `zlink_disconnect_rid()`. STREAM target routing ids must be 4 bytes.
- Connect/disconnect are **not** in-band data markers. They are reported
  through the socket monitor as `ZLINK_EVENT_CONNECTION_READY` /
  `ZLINK_EVENT_DISCONNECTED`, each carrying the 4-byte `routing_id`. A raw
  payload that happens to be a single `0x00`/`0x01` byte is delivered as
  ordinary data.

---

## 4. Callback Example

In STREAM raw callbacks every delivered part is application data; observe
connect/disconnect on the socket monitor (see [Monitoring](06-monitoring.en.md)).

```c
void on_message(const zlink_routing_id_t *source_rid,
                zlink_msg_t *parts, size_t part_count,
                void *userdata)
{
    for (size_t i = 0; i < part_count; i++) {
        void *data = zlink_msg_data(&parts[i]);
        size_t size = zlink_msg_size(&parts[i]);

        /* echo reply */
        zlink_msg_t reply;
        zlink_msg_init_size(&reply, size);
        memcpy(zlink_msg_data(&reply), data, size);
        zlink_send_rid(stream, source_rid, &reply, 1, 0);

        zlink_msg_close(&parts[i]);
    }
}

/* Attach callback dispatch (detachable outside the callback; detach/close from
   inside the callback returns EBUSY) */
zlink_recv_handler(stream, on_message, NULL);
```

### Key Points

| Item | Description |
|---|---|
| Attach API | `zlink_recv_handler()` |
| Callback | `zlink_socket_msg_handler_fn` |
| Lifetime | Detachable outside the callback; detach/close from inside the callback returns `EBUSY` |
| Framing | Raw bytes as received from the transport |
| Send | `zlink_send_rid()` |

> When the send queue is full (HWM), `zlink_send_rid()` blocks
> (default) or returns `ZLINK_SUBMIT_BACKPRESSURED` with `ZLINK_DONTWAIT`. For advanced
> backpressure patterns, see [Performance Guide](10-performance.en.md).

- Only one callback can be attached at a time; calling attach while a
  callback is already attached returns `ZLINK_HANDLER_BUSY`.
- The handler is permanent and cannot be detached for the lifetime of
  the socket.
- Close from inside the callback is not supported (returns `ZLINK_CLOSE_BUSY`).

---

## 4.1 Packet Callback Mode

When the upstream protocol uses the fixed framing convention (2-byte
big-endian header size + 4-byte big-endian body size + header payload +
body payload), register a packet-level callback with
`zlink_stream_packet_handler()`. The core handles fragment accumulation
and length parsing, so the application receives assembled header/body
pairs directly.

```c
void on_packet(void *stream,
               const zlink_routing_id_t *source_rid,
               zlink_msg_t *header,
               zlink_msg_t *body,
               void *userdata)
{
    /* header and body are always valid zlink_msg_t objects. Length zero
       is still delivered as a valid msg_t (never NULL). */
    /* source_rid is a borrowed view valid only for the duration of the
       callback. Copy the value if you need to keep it afterwards. */

    /* ... process header / body ... */

    zlink_msg_close(header);
    zlink_msg_close(body);
}

zlink_stream_packet_handler(stream, on_packet, NULL);
```

Rules for packet callback mode:

- `header_size` or `body_size` equal to zero is allowed; both sides are
  still delivered as valid `zlink_msg_t` objects.
- Ownership of `header` and `body` is transferred to the callback. The
  callback must close or consume each `msg_t` exactly once.
- With packet handler attached, raw recv (`zlink_recv()`), raw callback
  (`zlink_recv_handler()`), and data-plane `ZLINK_POLLIN` registration on
  the same handle all fail with `EBUSY`. A second
  `zlink_stream_packet_handler()` attach also fails with `EBUSY`.
- Malformed packets (length exceeding implementation limits, assembly
  failure, premature close, etc.) result in the connection being closed
  as the default policy. Observe such events via the socket monitor.

This mode relieves the application from re-implementing fragment
accumulation, but it does not change the fact that transport fragment
boundaries differ from packet boundaries.

---

## 5. Client Implementation Rule

Clients must be implemented as raw socket/websocket clients.

Conceptual POSIX TCP example (RAW mode — no zlink framing, just bytes):

```c
// RAW mode: send/recv raw bytes; message boundaries are application-defined
send(fd, body, body_len, 0);

char buf[4096];
ssize_t n = recv(fd, buf, sizeof(buf), 0);
```

If the server side uses the **packet handler** (`zlink_stream_packet_handler`),
the client must frame each packet as 2-byte BE header size + 4-byte BE body
size + header + body:

```c
// packet mode: [2B header_size BE][4B body_size BE][header][body]
uint16_t hsz_be = htons(header_len);
uint32_t bsz_be = htonl(body_len);
send(fd, &hsz_be, 2, 0);
send(fd, &bsz_be, 4, 0);
send(fd, header, header_len, 0);
send(fd, body, body_len, 0);
```

---

## 6. Option and Runtime Policy

Main supported options:
- `ZLINK_OPT_MAXMSGSIZE`, `ZLINK_OPT_SNDHWM`, `ZLINK_OPT_RCVHWM`, `ZLINK_OPT_SNDBUF`, `ZLINK_OPT_RCVBUF`, `ZLINK_OPT_BACKLOG`, `ZLINK_OPT_LINGER`
- `ZLINK_STREAM_OPT_NOTIFY` (via `zlink_set_stream_option()` / `zlink_get_stream_option()`): enable connect/disconnect notifications
- TLS/WSS server: `zlink_set_tls_server()` / TLS client: `zlink_set_tls_client()`

STREAM listeners often receive bytes from raw TCP peers. If the peer is not
fully trusted, set `ZLINK_OPT_MAXMSGSIZE` to the largest application message you
intend to accept before calling `zlink_bind`. Without that setting the
compatibility default is unlimited.

Unsupported/changed:
- Setting `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID` on STREAM returns `EOPNOTSUPP`.

### 6.1 Default STREAM runtime profile

Defaults currently used by STREAM internals:
- `ZLINK_OPT_BACKLOG`: `65536`
- `ZLINK_OPT_SNDHWM` / `ZLINK_OPT_RCVHWM`: STREAM profile byte value from the default balanced auto-HWM policy, or the manual byte default if context auto-HWM is disabled
- `ZLINK_OPT_SNDBUF` / `ZLINK_OPT_RCVBUF`: default `-1`, leaving OS buffer defaults and TCP autotuning in control
- STREAM batch size default: `4096`
- STREAM read headroom default: `64`
- STREAM accept concurrency default: `4` (clamped to max `128`)
- STREAM session scheduling default: `rr`

> STREAM runtime environment variables and internal tuning constants
> are documented in [STREAM internals](../internals/stream-socket.en.md).

---

## 7. Errors and Constraints

- `zlink_connect(stream, ...)` -> `EOPNOTSUPP`
- On STREAM, non-4-byte `routing_id` frame is a protocol error
- Messages larger than `MAXMSGSIZE` are dropped and connection is closed (disconnect event)

---

## 8. Reference Tests

- `core/tests/integration/test_stream_socket.cpp`
- `core/tests/integration/test_stream_fastpath.cpp`
- `core/tests/integration/routing-id/test_connect_rid_string_alias.cpp`
- `core/tests/scenario/stream/zlink/test_scenario_stream_zlink.cpp`

These tests use STREAM server + raw client paths.

---
[← ROUTER](03-4-router.en.md) | [Proxy →](03-6-proxy.en.md) | [Transport →](04-transports.en.md)


## Full language examples

=== "C++"

    ```cpp
    --8<-- "bindings/cpp/samples/stream_recv_sample.cpp:doc"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "bindings/dotnet/samples/StreamRecv/Program.cs:doc"
    ```

=== "Java"

    ```java
    --8<-- "bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/StreamRecvSample.java:doc"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "bindings/kotlin/samples/src/main/kotlin/systems/zlink/samples/StreamRecvSample.kt:doc"
    ```

=== "Python"

    ```python
    --8<-- "bindings/python/samples/stream_recv_sample.py:doc"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "bindings/node/samples/stream_recv_sample.ts:doc"
    ```

=== "JavaScript"

    ```javascript
    --8<-- "bindings/javascript/samples/stream_recv_sample.js:doc"
    ```

=== "Go"

    ```go
    --8<-- "bindings/go/samples/stream_recv_sample/main.go:doc"
    ```

=== "Rust"

    ```rust
    --8<-- "bindings/rust/samples/stream_recv_sample.rs:doc"
    ```

---
<!-- zlink-nav:bottom:start -->
[← ROUTER](03-4-router.en.md) | [Proxy →](03-6-proxy.en.md)
<!-- zlink-nav:bottom:end -->
