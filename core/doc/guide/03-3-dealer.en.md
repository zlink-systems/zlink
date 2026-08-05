[English](03-3-dealer.en.md) | [한국어](03-3-dealer.ko.md)

<!-- zlink-nav:start -->
[← PUB/SUB](03-2-pubsub.en.md) | [ROUTER →](03-4-router.en.md)
<!-- zlink-nav:end -->

# DEALER Socket

## 1. Overview

The DEALER socket is an asynchronous request socket. It sends to multiple peers using **round-robin** distribution and receives using **fair-queue**. There is no enforced send/recv ordering, enabling free asynchronous messaging.

**Key characteristics:**
- Send: Round-robin -- cyclic distribution across connected peers
- Receive: Fair-queue -- fair reception from all peers
- No enforced send/recv ordering (asynchronous)

**Valid socket combinations:** DEALER ↔ ROUTER, DEALER ↔ DEALER

```mermaid
flowchart LR
    D1[DEALER 1] -->|round-robin| R[ROUTER]
    D2[DEALER 2] -->|round-robin| R
```

### Concrete Scenario: 3 DEALERs Sending to 1 ROUTER

Three DEALER clients connect to a single ROUTER server. Each DEALER sends
requests independently; the ROUTER receives them via fair-queue and
distinguishes each sender by `source_rid`.

| Sender | routing_id | Message | ROUTER sees |
|--------|-----------|---------|-------------|
| DEALER 1 | `D1` | `"buy AAPL 100"` | source_rid=`D1`, data=`"buy AAPL 100"` |
| DEALER 2 | `D2` | `"sell TSLA 50"` | source_rid=`D2`, data=`"sell TSLA 50"` |
| DEALER 3 | `D3` | `"buy MSFT 200"` | source_rid=`D3`, data=`"buy MSFT 200"` |

The ROUTER replies to each DEALER using `zlink_send_rid()` with the
corresponding `source_rid`. Because DEALER uses round-robin for
*outgoing* connections, if a single DEALER connects to multiple ROUTERs,
its messages cycle across them (msg1 -> ROUTER-A, msg2 -> ROUTER-B, ...).

## 2. Basic Usage

### Creation and Connection

```c
void *dealer = zlink_socket(ctx, ZLINK_SOCKET_DEALER);

/* Set routing_id (optional, used for identification by ROUTER) */
zlink_set_routing_id(dealer, "client-1", 8);

/* Connect to server */
zlink_connect(dealer, "tcp://127.0.0.1:5558");
```

### Sending and Receiving Messages

```c
/* Send requests -- can send consecutively without ordering constraints */
zlink_msg_t msg1, msg2, msg3;
zlink_msg_init_size(&msg1, 9);
memcpy(zlink_msg_data(&msg1), "request-1", 9);
zlink_send(dealer, &msg1, 1, 0);

zlink_msg_init_size(&msg2, 9);
memcpy(zlink_msg_data(&msg2), "request-2", 9);
zlink_send(dealer, &msg2, 1, 0);

zlink_msg_init_size(&msg3, 9);
memcpy(zlink_msg_data(&msg3), "request-3", 9);
zlink_send(dealer, &msg3, 1, 0);

/* Responses are drained with zlink_recv() in a poller loop,
   or (for zlink_dealer_request()) delivered through its reply callback */
```

### Receive Modes

Use `zlink_recv()` to receive synchronously.

```c
zlink_routing_id_t source_rid;
zlink_msg_t *parts = NULL;
size_t part_count = 0;
zlink_recv_result_t rc = zlink_recv(
    dealer, &source_rid, &parts, &part_count, 0 /* flags */);
if (rc == ZLINK_RECV_OK) {
    /* process parts[0..part_count-1] */
    zlink_multipart_close(parts, part_count);
}
/* other rc values: ZLINK_RECV_NO_DATA (EAGAIN),
   TERMINATED, INVALID_HANDLE, NOT_SUPPORTED */
```

> When HWM is reached, `zlink_send()` blocks (default) or returns
> `ZLINK_SUBMIT_BACKPRESSURED` with `ZLINK_DONTWAIT`. For advanced
> backpressure patterns, see [Performance Guide](10-performance.en.md).

## 3. Usage Example

```c
/* DEALER → ROUTER send */
zlink_msg_t parts[2];
zlink_msg_init_size(&parts[0], 6);
memcpy(zlink_msg_data(&parts[0]), "header", 6);
zlink_msg_init_size(&parts[1], 4);
memcpy(zlink_msg_data(&parts[1]), "body", 4);
zlink_send(dealer, parts, 2, 0);
```

## 4. Socket Options

| Option | Type | Default | Description |
|------|------|--------|------|
| `zlink_set_routing_id()` | binary | Auto (UUID) | ID for identification by ROUTER (dedicated function) |
| `ZLINK_DEALER_OPT_PROBE` | int | 0 | Send empty message on connect (connection notification) |
| `ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS` | int | 0 | Default timeout for `zlink_dealer_request()`. `0` uses the implementation default of `5000ms` |
| `ZLINK_DEALER_OPT_WEIGHT` | int | 100 | Per-peer load-balancing weight for outgoing round-robin |
| `ZLINK_OPT_SNDHWM` | `uint64_t` bytes | automatic | Auto-HWM sized for DEALER's peer-queue role. Manual settings take precedence; `0` is unlimited |
| `ZLINK_OPT_RCVHWM` | `uint64_t` bytes | automatic | Auto-HWM sized for DEALER's peer-queue role. Manual settings take precedence; `0` is unlimited |
| `ZLINK_OPT_LINGER` | int | -1 | Wait time on close (ms) |
| `ZLINK_OPT_SNDTIMEO` | int | 1000 | Send timeout (ms); set `-1` explicitly for infinite wait |
| `ZLINK_OPT_RCVTIMEO` | int | 1000 | Receive timeout (ms); set `-1` explicitly for infinite wait |

### Setting routing_id

To allow ROUTER to identify a DEALER, explicitly set the routing_id.

```c
/* Set before bind/connect */
zlink_set_routing_id(dealer, "D1", 2);
zlink_connect(dealer, "tcp://127.0.0.1:5558");
```

> Reference: `core/tests/integration/test_router_multiple_dealers.cpp` -- `zlink_set_routing_id(dealer1, "D1", 2)`

### 4.1 Request-Reply

When DEALER needs to send a request and wait for a reply, use
`zlink_dealer_request()` instead of ordinary `send/recv`. This function
attaches a ZMP request-reply envelope and delivers the reply via callback.

> For the ZMP request-reply envelope wire format, see
> [ZMP Protocol](../internals/protocol-zmp.en.md).

```c
static void on_reply(zlink_request_result_t result,
                     zlink_msg_t *parts,
                     size_t part_count,
                     void *userdata)
{
    if (result != ZLINK_REQUEST_OK) {
        /* result values: ZLINK_REQUEST_TIMED_OUT, NOT_FOUND,
           TERMINATED, PROTOCOL_ERROR */
        fprintf(stderr, "request failed: %d\n", (int)result);
        return;
    }

    for (size_t i = 0; i < part_count; ++i)
        zlink_msg_close(&parts[i]);
}

int timeout_ms = 1000;
zlink_set_dealer_option(
  dealer,
  ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS,
  &timeout_ms,
  sizeof(timeout_ms));

zlink_msg_t req;
zlink_msg_init_size(&req, 4);
memcpy(zlink_msg_data(&req), "ping", 4);
/* signature: zlink_dealer_request(dealer, parts, count, handler,
   userdata, flags, timeout_ms) */
zlink_submit_result_t rc = zlink_dealer_request(
    dealer, &req, 1, on_reply, NULL, 0 /* flags */, 0 /* timeout_ms */);
if (rc != ZLINK_SUBMIT_OK) { /* handle submit failure */ }
```

When `timeout_ms=0` is passed to `zlink_dealer_request()`, it uses the
socket default. If the socket default is also `0`, the implementation
default of `5000ms` applies.

## 5. Usage Patterns

### Pattern 1: DEALER → ROUTER Request-Reply

The most basic pattern. DEALER sends requests, ROUTER replies.

```c
/* Server: ROUTER with handler */
void on_request(const zlink_routing_id_t *source_rid,
                uint64_t request_seq,
                zlink_msg_t *parts, size_t part_count,
                void *userdata)
{
    /* source_rid contains the DEALER's routing_id;
       request_seq is non-zero for request-reply, 0 for one-way */
    printf("Received from [%.*s]: %.*s\n",
           (int)source_rid->size, source_rid->data,
           (int)zlink_msg_size(&parts[0]),
           (char *)zlink_msg_data(&parts[0]));

    /* Correlated reply: send back to the source peer with its request_seq */
    zlink_msg_t reply;
    zlink_msg_init_size(&reply, 5);
    memcpy(zlink_msg_data(&reply), "World", 5);
    zlink_router_reply(router, source_rid, request_seq, &reply, 1, 0);

    for (size_t i = 0; i < part_count; i++)
        zlink_msg_close(&parts[i]);
}

void *router = zlink_socket(ctx, ZLINK_SOCKET_ROUTER);
/* Receive with zlink_router_recv() (source_rid + request_seq) */
zlink_bind(router, "tcp://*:5558");

/* Client: DEALER */
void *dealer = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
/* Receive replies with zlink_recv() */
zlink_set_routing_id(dealer, "D1", 2);
zlink_connect(dealer, "tcp://127.0.0.1:5558");

/* Client request */
zlink_msg_t req;
zlink_msg_init_size(&req, 5);
memcpy(zlink_msg_data(&req), "Hello", 5);
zlink_send(dealer, &req, 1, 0);

/* on_request receives the message, replies with "World"
   on_reply receives the reply */
```

> Reference: `core/tests/integration/test_router_multiple_dealers.cpp` -- TCP/IPC/inproc examples

### Pattern 2: Multiple DEALER Load Balancing

Multiple DEALERs connect to a single ROUTER. ROUTER distinguishes each DEALER by routing_id.

```c
void *router = zlink_socket(ctx, ZLINK_SOCKET_ROUTER);
/* ROUTER receives with zlink_recv() and distinguishes each DEALER by source_rid */
zlink_bind(router, "tcp://127.0.0.1:*");

char endpoint[256];
size_t len = sizeof(endpoint);
zlink_get_option(router, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len);

void *dealer1 = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
zlink_set_routing_id(dealer1, "D1", 2);
zlink_connect(dealer1, endpoint);

void *dealer2 = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
zlink_set_routing_id(dealer2, "D2", 2);
zlink_connect(dealer2, endpoint);

/* Each DEALER sends a message */
zlink_msg_t m1;
zlink_msg_init_size(&m1, 12);
memcpy(zlink_msg_data(&m1), "from_dealer1", 12);
zlink_send(dealer1, &m1, 1, 0);

zlink_msg_t m2;
zlink_msg_init_size(&m2, 12);
memcpy(zlink_msg_data(&m2), "from_dealer2", 12);
zlink_send(dealer2, &m2, 1, 0);

/* on_message receives each DEALER's message with its routing_id */
```

> Reference: `core/tests/integration/test_router_multiple_dealers.cpp` -- `test_router_multiple_dealers_tcp()`

### Pattern 3: Proxy Pattern (ROUTER-DEALER)

Build a multi-threaded server using ROUTER (frontend) + DEALER (backend).

```c
/* Frontend: clients connect here */
void *frontend = zlink_socket(ctx, ZLINK_SOCKET_ROUTER);
zlink_bind(frontend, "tcp://*:5558");

/* Backend: worker threads connect here */
void *backend = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
zlink_bind(backend, "inproc://backend");

/* Start worker threads then run proxy */
zlink_proxy(frontend, backend, NULL);
```

```c
/* Worker thread */
void worker_thread(void *arg) {
    void on_work(const zlink_routing_id_t *source_rid,
                 zlink_msg_t *parts, size_t part_count,
                 void *userdata)
    {
        /* Process and reply with the same routing_id */
        zlink_send_rid(worker, source_rid, parts, part_count, 0);
    }

    void *worker = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
    /* Receive work with zlink_recv() */
    zlink_connect(worker, "inproc://backend");

    /* Worker stays alive until socket is closed */
}
```

> Reference: `core/tests/integration/test_proxy.cpp` -- ROUTER(frontend) + DEALER(backend) + worker pool

### Pattern 4: DEALER ↔ DEALER Asynchronous Communication

Both sides use DEALER for fully asynchronous P2P communication.

```c
void *a = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
/* Receive with zlink_recv() */
zlink_bind(a, "tcp://*:5558");

void *b = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
/* Receive with zlink_recv() */
zlink_connect(b, "tcp://127.0.0.1:5558");

/* Bidirectional free send */
zlink_msg_t ping;
zlink_msg_init_size(&ping, 4);
memcpy(zlink_msg_data(&ping), "ping", 4);
zlink_send(a, &ping, 1, 0);

zlink_msg_t pong;
zlink_msg_init_size(&pong, 4);
memcpy(zlink_msg_data(&pong), "pong", 4);
zlink_send(b, &pong, 1, 0);

/* on_message_b receives "ping", on_message_a receives "pong" */
```

## 6. Caveats

### No Peer Connected vs. HWM Backpressure

These are two distinct results. When **no peer** is connected (no
positive-weight pipe), a send returns `ZLINK_SUBMIT_NOT_ADMITTED` — the
message is not queued. When a peer **is** connected but its send queue has
reached the HWM, the call blocks (default) or returns
`ZLINK_SUBMIT_BACKPRESSURED` with `ZLINK_DONTWAIT`.

```c
/* Send with no peer connected */
zlink_msg_t msg;
zlink_msg_init_size(&msg, 4);
memcpy(zlink_msg_data(&msg), "data", 4);
zlink_submit_result_t rc = zlink_send(dealer, &msg, 1, ZLINK_DONTWAIT);
if (rc == ZLINK_SUBMIT_NOT_ADMITTED) {
    /* No connected peer to admit the message */
} else if (rc == ZLINK_SUBMIT_BACKPRESSURED) {
    /* A peer is connected but its queue is at the HWM */
}
```

### Round-Robin Distribution

When multiple peers are connected, messages are distributed in a round-robin fashion. To send to a specific peer, use ROUTER instead.

### Weight-Aware Outbound Selection

Remote peers advertise a weight (`0..10000`). DEALER automatically drops
weight-`0` peers from its candidate set. Positive peers remain eligible,
and unequal positive weights change the send ratio. The underlying
connections stay alive, so a peer that flips back to a positive weight
rejoins the rotation without reconnect.

If every known peer is `0`, `zlink_send()` and
`zlink_dealer_request()` return `ZLINK_SUBMIT_NOT_ADMITTED`. The caller
should wait for at least one peer to return to a positive weight before
retrying; treating `NOT_ADMITTED` as a hard failure would discard
messages that are expected to succeed once maintenance ends.

> For the full contract, see
> [Weight-aware outbound selection](../spec/core/socket/06-dealer.en.md#2-dealer-options)
> in the DEALER spec.

### Set routing_id Before connect

`zlink_set_routing_id()` must be called before `zlink_connect()`. Changes after connection are not applied.

```c
/* Correct order */
zlink_set_routing_id(dealer, "D1", 2);
zlink_connect(dealer, endpoint);  /* identified as D1 */
```

---
[← PUB/SUB](03-2-pubsub.en.md) | [ROUTER →](03-4-router.en.md)


## Full language examples

=== "C++"

    ```cpp
    --8<-- "bindings/cpp/samples/dealer_router_recv_sample.cpp:doc"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "bindings/dotnet/samples/DealerRouterRecv/Program.cs:doc"
    ```

=== "Java"

    ```java
    --8<-- "bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/DealerRouterRecvSample.java:doc"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "bindings/kotlin/samples/src/main/kotlin/systems/zlink/samples/DealerRouterRecvSample.kt:doc"
    ```

=== "Python"

    ```python
    --8<-- "bindings/python/samples/dealer_router_recv_sample.py:doc"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "bindings/node/samples/dealer_router_recv_sample.ts:doc"
    ```

=== "JavaScript"

    ```javascript
    --8<-- "bindings/javascript/samples/dealer_router_recv_sample.js:doc"
    ```

=== "Go"

    ```go
    --8<-- "bindings/go/samples/dealer_router_recv_sample/main.go:doc"
    ```

=== "Rust"

    ```rust
    --8<-- "bindings/rust/samples/dealer_router_recv_sample.rs:doc"
    ```

---
<!-- zlink-nav:bottom:start -->
[← PUB/SUB](03-2-pubsub.en.md) | [ROUTER →](03-4-router.en.md)
<!-- zlink-nav:bottom:end -->
