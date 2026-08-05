[English](03-1-pair.en.md) | [한국어](03-1-pair.ko.md)

<!-- zlink-nav:start -->
[← Socket Patterns](03-0-socket-patterns.en.md) | [PUB/SUB →](03-2-pubsub.en.md)
<!-- zlink-nav:end -->

# PAIR Socket

## 1. Overview

The PAIR socket forms an exclusive 1:1 bidirectional connection with exactly one peer. If a second peer connects, that later connection is rejected — the first peer keeps the pipe.

**Key characteristics:**
- Only a single pipe is allowed (1:1 exclusive)
- Bidirectional free messaging (send/recv order does not matter)
- The simplest socket type

**Valid socket combinations:** PAIR ↔ PAIR

```mermaid
flowchart LR
    A[PAIR A] <-->|Bidirectional| B[PAIR B]
```

## 2. Basic Usage

### Creation and Connection

```c
void *ctx = zlink_ctx_new();

/* Server side */
void *server = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(server, "tcp://*:5555");

/* Client side */
void *client = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(client, "tcp://127.0.0.1:5555");
```

### Message Exchange

PAIR is a recv-only type: receive is performed with `zlink_recv()`,
typically inside a poller loop. Both peers can send and receive freely.

```c
/* Client → Server */
zlink_msg_t msg;
zlink_msg_init_size(&msg, 5);
memcpy(zlink_msg_data(&msg), "Hello", 5);
zlink_send(client, &msg, 1, 0);

/* Server receives with zlink_recv() (typically inside a poller loop) */
zlink_routing_id_t source_rid;
zlink_msg_t *parts = NULL;
size_t part_count = 0;
if (zlink_recv(server, &source_rid, &parts, &part_count, 0) == ZLINK_RECV_OK) {
    printf("Received: %.*s\n",
           (int)zlink_msg_size(&parts[0]),
           (char *)zlink_msg_data(&parts[0]));
    zlink_multipart_close(parts, part_count);
}

/* Server → Client (bidirectional; client uses the same recv+poller pattern) */
zlink_msg_t reply;
zlink_msg_init_size(&reply, 5);
memcpy(zlink_msg_data(&reply), "World", 5);
zlink_send(server, &reply, 1, 0);
```

### Sending Multipart Data

Multipart data is sent as a parts array in a single `zlink_send` call.

```c
zlink_msg_t parts[2];
zlink_msg_init_size(&parts[0], 3);
memcpy(zlink_msg_data(&parts[0]), "foo", 3);
zlink_msg_init_size(&parts[1], 6);
memcpy(zlink_msg_data(&parts[1]), "foobar", 6);
zlink_send(server, parts, 2, 0);

/* Receiver pulls both frames from one zlink_recv() call:
   parts[0] = "foo", parts[1] = "foobar", part_count = 2 */
```

> Reference: `core/tests/integration/test_pair_inproc.cpp` -- `test_zlink_send_multipart()` test

### Receive Modes

PAIR is recv/poller-only in the public API.
Use `zlink_recv()` to receive synchronously.

```c
void *pair = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(pair, "tcp://*:5556");

zlink_routing_id_t source_rid;
zlink_msg_t *parts = NULL;
size_t part_count = 0;
zlink_recv_result_t rc = zlink_recv(
    pair, &source_rid, &parts, &part_count, 0 /* flags */);
if (rc == ZLINK_RECV_OK) {
    /* process parts[0..part_count-1] */
    zlink_multipart_close(parts, part_count);
}
```

> When HWM is reached, `zlink_send()` blocks (default) or returns
> `ZLINK_SUBMIT_BACKPRESSURED` with `ZLINK_DONTWAIT`. For advanced
> backpressure patterns,
> see [Performance Guide](10-performance.en.md).

## 3. Message Format

PAIR socket message frames contain **application data only**.

```
Single frame:     [data]
Multipart frame:  [frame1][frame2]...[frameN]
```

> For `source_rid` and the common receive interface, see
> [Socket Patterns Overview](03-0-socket-patterns.en.md#common-receive-model).

Multipart send:

```c
zlink_msg_t parts[2];
zlink_msg_init_size(&parts[0], 6);
memcpy(zlink_msg_data(&parts[0]), "header", 6);
zlink_msg_init_size(&parts[1], 4);
memcpy(zlink_msg_data(&parts[1]), "body", 4);
zlink_send(server, parts, 2, 0);
```

## 4. Socket Options

| Option | Type | Default | Description |
|------|------|--------|------|
| `ZLINK_OPT_SNDHWM` | `uint64_t` bytes | automatic | Auto-HWM sized for PAIR's peer-queue role. Manual settings take precedence; `0` is unlimited |
| `ZLINK_OPT_RCVHWM` | `uint64_t` bytes | automatic | Auto-HWM sized for PAIR's peer-queue role. Manual settings take precedence; `0` is unlimited |
| `ZLINK_OPT_LINGER` | int | -1 | Wait time for unsent messages on close (ms), -1=infinite |
| `ZLINK_OPT_SNDTIMEO` | int | 1000 | Send timeout (ms); set `-1` explicitly for infinite wait |
| `ZLINK_OPT_RCVTIMEO` | int | 1000 | Receive timeout (ms); set `-1` explicitly for infinite wait |

```c
uint64_t hwm_bytes = 5 * 1024 * 1024;  /* HWM is bytes, passed as exactly 8 bytes */
zlink_set_option(socket, ZLINK_OPT_SNDHWM, &hwm_bytes, sizeof(hwm_bytes));

int linger = 0;  /* return immediately on close */
zlink_set_option(socket, ZLINK_OPT_LINGER, &linger, sizeof(linger));
```

## 5. Usage Patterns

### Pattern 1: Inter-thread Signaling (inproc)

The most common PAIR use case. Zero-copy communication between threads via the inproc transport.

```c
/* Main thread */
void *signal = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(signal, "inproc://signal");

/* Worker thread */
void *worker_signal = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(worker_signal, "inproc://signal");

/* Worker → Main: task completion signal */
zlink_msg_t msg;
zlink_msg_init_size(&msg, 4);
memcpy(zlink_msg_data(&msg), "DONE", 4);
zlink_send(worker_signal, &msg, 1, 0);

/* Main: receives "DONE" via its poller loop (zlink_recv) */
```

> Reference: `core/tests/integration/test_pair_inproc.cpp` -- bind → connect → bounce pattern

### Pattern 2: TCP Communication

1:1 communication over the network. Wildcard bind enables automatic port assignment.

```c
/* Server: wildcard port */
void *server = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(server, "tcp://127.0.0.1:*");

/* Query the assigned endpoint */
char endpoint[256];
size_t len = sizeof(endpoint);
zlink_get_option(server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len);

/* Client: connect using the queried endpoint */
void *client = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(client, endpoint);
```

> Reference: `core/tests/integration/test_pair_tcp.cpp` -- `bind_loopback_ipv4()` + wildcard bind

### Pattern 3: Connection by DNS Name

You can also connect using a hostname.

```c
void *client = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(client, "tcp://localhost:5555");
```

> Reference: `core/tests/integration/test_pair_tcp.cpp` -- `test_pair_tcp_connect_by_name()`

### Pattern 4: IPC Communication

Inter-process communication on the same machine (Linux/macOS).

```c
void *server = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(server, "ipc:///tmp/myapp.ipc");

void *client = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(client, "ipc:///tmp/myapp.ipc");
```

> Reference: `core/tests/integration/test_pair_ipc.cpp` -- includes IPC path length validation

## 6. Caveats

### Only a Single Peer Allowed

A PAIR socket maintains only one connection. If a second peer connects, that later connection is rejected; the first peer keeps the pipe.

```
 Allowed:  PAIR A ↔ PAIR B      (1:1)
 Invalid:  PAIR A ← PAIR B      (N:1 attempt: later peers rejected)
               ← PAIR C
```

Use DEALER/ROUTER if N:1 communication is needed.

### inproc bind Order

With the inproc transport, **bind must be called before connect**.

```c
/* Correct order */
zlink_bind(a, "inproc://signal");     /* 1. bind first */
zlink_connect(b, "inproc://signal");  /* 2. connect */

/* Wrong order -- fails */
zlink_connect(b, "inproc://signal");  /* fails because bind has not been called yet */
zlink_bind(a, "inproc://signal");
```

### IPC Path Length

The file path of an IPC endpoint cannot exceed the system limit (typically 108 characters).

```c
/* Path too long → ENAMETOOLONG error */
zlink_bind(socket, "ipc:///very/long/path/.../endpoint.ipc");
```

> Reference: `core/tests/integration/test_pair_ipc.cpp` -- `test_endpoint_too_long()`

### HWM Behavior

When there is no peer or the peer is slow, outgoing messages are queued up to the HWM. When the HWM is exceeded, `zlink_send()` blocks (default) or returns `ZLINK_SUBMIT_BACKPRESSURED` (`ZLINK_DONTWAIT`).

### LINGER Setting

When `zlink_close()` is called and there are unsent messages remaining, it waits for the LINGER duration. For tests or when a fast shutdown is needed:

```c
int linger = 0;
zlink_set_option(socket, ZLINK_OPT_LINGER, &linger, sizeof(linger));
```

---
[← Socket Patterns](03-0-socket-patterns.en.md) | [PUB/SUB →](03-2-pubsub.en.md)


## Full language examples

=== "C++"

    ```cpp
    --8<-- "bindings/cpp/samples/pair_recv_sample.cpp:doc"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "bindings/dotnet/samples/PairRecv/Program.cs:doc"
    ```

=== "Java"

    ```java
    --8<-- "bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/PairRecvSample.java:doc"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "bindings/kotlin/samples/src/main/kotlin/systems/zlink/samples/PairRecvSample.kt:doc"
    ```

=== "Python"

    ```python
    --8<-- "bindings/python/samples/pair_recv_sample.py:doc"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "bindings/node/samples/pair_recv_sample.ts:doc"
    ```

=== "JavaScript"

    ```javascript
    --8<-- "bindings/javascript/samples/pair_recv_sample.js:doc"
    ```

=== "Go"

    ```go
    --8<-- "bindings/go/samples/pair_recv_sample/main.go:doc"
    ```

=== "Rust"

    ```rust
    --8<-- "bindings/rust/samples/pair_recv_sample.rs:doc"
    ```

---
<!-- zlink-nav:bottom:start -->
[← Socket Patterns](03-0-socket-patterns.en.md) | [PUB/SUB →](03-2-pubsub.en.md)
<!-- zlink-nav:bottom:end -->
