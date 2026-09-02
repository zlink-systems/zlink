
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
  length-delimited packets, use PACKET mode, which frames as
  2-byte BE header size + 4-byte BE body size + header + body.
- At the zlink API level: raw `zlink_recv_part()` exposes the source
  client's 4-byte `routing_id` through its own `source_rid_out_`
  out-parameter, and packet `zlink_stream_recv_packet()` exposes the same
  Core-owned borrowed view through `source_rid_out_`.

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
two receive modes must be selected before the first successful bind.

- **RAW**: `zlink_recv_part()` pulls transport fragments directly, one
  part at a time, with the source routing id returned through its
  `source_rid_out_` out-parameter. Pair it with a poller watching
  `ZLINK_POLLIN`.
- **PACKET**: `zlink_stream_recv_packet()` pulls packets
  assembled from a fixed framing convention (2B header size + 4B body
  size + header + body, all big-endian) as header/body pairs.

Set `ZLINK_STREAM_OPT_RECV_MODE` to `ZLINK_STREAM_RECV_MODE_RAW` or
`ZLINK_STREAM_RECV_MODE_PACKET` before bind. The mode becomes immutable after
the first successful bind; the receive API for the other mode returns `ENOTSUP`.

STREAM-specific behavior:

- `source_rid` is auto-assigned per connection by the server,
  always fixed 4 bytes (`uint32`, big-endian).
- To close one client, pass the `source_rid` received from recv
  to `zlink_disconnect_rid()`. STREAM target routing ids must be 4 bytes.
- Connect/disconnect are **not** in-band data markers. They are reported
  through the socket monitor as `ZLINK_EVENT_CONNECTION_READY` /
  `ZLINK_EVENT_DISCONNECTED`, each carrying the 4-byte `routing_id`. A raw
  payload that happens to be a single `0x00`/`0x01` byte is delivered as
  ordinary data.

---

## 4. RAW Pull Example

In STREAM RAW mode every pulled part is application data; observe
connect/disconnect on the socket monitor (see [Monitoring](../spec/core/06-monitoring.en.md)).

```c
zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
zlink_set_stream_option(stream, ZLINK_STREAM_OPT_RECV_MODE,
                        &mode, sizeof(mode));
zlink_bind(stream, "tcp://0.0.0.0:8080");

const zlink_routing_id_t *source_rid = NULL;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t more = ZLINK_PART_FINAL;
if (zlink_recv_part(stream, &source_rid, &part, &more,
                    ZLINK_RECV_FLAGS_NONE) == ZLINK_RECV_OK) {
    zlink_msg_t reply;
    zlink_msg_init_size(&reply, zlink_msg_size(&part));
    memcpy(zlink_msg_data(&reply), zlink_msg_data(&part), zlink_msg_size(&part));
    zlink_send_part_rid(stream, source_rid, &reply, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL);
    zlink_msg_close(&part);
}
```

### Key Points

| Item | Description |
|---|---|
| Receive API | `zlink_recv_part()` |
| Readiness | A poller reports `ZLINK_POLLIN`; the application then drains receives |
| Lifetime | `source_rid` remains valid until the same socket's next data-recv entry or close |
| Framing | Raw bytes as received from the transport |
| Send | `zlink_send_part_rid()` |

> When the send queue is full (HWM), `zlink_send_part_rid()` blocks
> (default) or returns `ZLINK_SUBMIT_BACKPRESSURED` with `ZLINK_DONTWAIT`. For advanced
> backpressure patterns, see [Performance Guide](10-performance.en.md).

- The caller owns a successfully received `zlink_msg_t` and closes it exactly once.
- Copy the borrowed `source_rid` before the next data receive when it must be retained.
- `ZLINK_RECV_FLAGS_DONTWAIT` returns `ZLINK_RECV_NO_DATA` with `EAGAIN` when empty.

---

## 4.1 PACKET Pull Mode

When the upstream protocol uses the fixed framing convention (2-byte
big-endian header size + 4-byte big-endian body size + header payload +
body payload), select PACKET mode and pull with
`zlink_stream_recv_packet()`. Core handles fragment accumulation and length
parsing, so the application receives assembled header/body pairs directly.

```c
zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
zlink_set_stream_option(stream, ZLINK_STREAM_OPT_RECV_MODE,
                        &mode, sizeof(mode));
zlink_bind(stream, "tcp://0.0.0.0:8080");

const zlink_routing_id_t *source_rid = NULL;
zlink_msg_t header;
zlink_msg_t body;
zlink_msg_init(&header);
zlink_msg_init(&body);
if (zlink_stream_recv_packet(stream, &source_rid, &header, &body,
                             ZLINK_RECV_FLAGS_NONE) == ZLINK_RECV_OK) {
    /* process header/body; zero-length messages are valid */
    zlink_msg_close(&header);
    zlink_msg_close(&body);
}
```

Rules for PACKET mode:

- `header_size` or `body_size` equal to zero is allowed; both sides are
  still delivered as valid `zlink_msg_t` objects.
- Ownership of `header` and `body` is transferred to the caller. The
  caller must close or consume each `msg_t` exactly once.
- In PACKET mode, raw receive (`zlink_recv_part()`) fails with `ENOTSUP`.
  In RAW mode, `zlink_stream_recv_packet()` fails the same way.
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

If the server side uses **PACKET mode** (`zlink_stream_recv_packet`),
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
- `ZLINK_STREAM_OPT_RECV_MODE` (via `zlink_set_stream_option()` / `zlink_get_stream_option()`): select RAW or PACKET before bind
- `ZLINK_STREAM_OPT_NOTIFY`: enable zero-length connect/disconnect records in RAW mode; it cannot be combined with PACKET mode
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
> are documented in [STREAM internals](../spec/core/socket/08-stream.en.md).

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

### PACKET pull examples

These variants pull packets that use the fixed framing convention.

=== "C++"

    ```cpp
    --8<-- "bindings/cpp/samples/stream_packet_pull_sample.cpp:doc"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "bindings/dotnet/samples/StreamPacketCallback/Program.cs:doc"
    ```

=== "Java"

    ```java
    --8<-- "bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/StreamPacketCallbackSample.java:doc"
    ```

=== "Kotlin"

    A current pull-based Kotlin PACKET sample is not yet available in this source tree.

=== "Python"

    ```python
    --8<-- "bindings/python/samples/stream_packet_recv_sample.py:doc"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "bindings/node/samples/stream_packet_sample.ts:doc"
    ```

=== "JavaScript"

    A current pull-based JavaScript PACKET sample is not yet available in this source tree.

=== "Go"

    ```go
    --8<-- "bindings/go/samples/stream_packet_callback_sample/main.go:doc"
    ```

=== "Rust"

    ```rust
    --8<-- "bindings/rust/samples/stream_packet_recv_sample.rs:doc"
    ```

---
<!-- zlink-nav:bottom:start -->
[← ROUTER](03-4-router.en.md) | [Proxy →](03-6-proxy.en.md)
<!-- zlink-nav:bottom:end -->
