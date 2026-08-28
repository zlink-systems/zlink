
<!-- zlink-nav:start -->
[← STREAM](03-5-stream.en.md) | [Transport →](04-transports.en.md)
<!-- zlink-nav:end -->

# Proxy Pattern

> **What this chapter answers** — how to compose ROUTER, DEALER, PAIR, and the
> PUB-family sockets into a proxy that relays messages. Each individual
> socket's contract is owned by that socket's own spec.

## 1. Overview

A proxy relays messages between two sockets.
`zlink_proxy()` is a general-purpose utility that works with any socket
combination. Users can also build custom proxies by composing the public
per-socket APIs.

## 2. zlink_proxy() — Built-in Proxy

```c
zlink_config_result_t zlink_proxy (void *frontend, void *backend, void *capture);
```

- Forwards messages from `frontend` to `backend` and vice versa
- If `capture` is non-NULL, copies all passing messages to the capture socket
- **Blocking function** — run in a dedicated thread
- **No socket type restriction** — internally uses the same `socket_base_t`
  recv/send paths as the raw socket APIs, so pairing sockets whose public
  send or receive surface returns `ZLINK_SUBMIT_NOT_SUPPORTED` /
  `ZLINK_RECV_NOT_SUPPORTED` (for example XSUB or XPUB) still works inside a
  proxy

### Example Socket Combinations

| frontend | backend | Use case |
|----------|---------|----------|
| XSUB | XPUB | PUB/SUB relay (most common) |
| ROUTER | DEALER | Request/reply broker |
| DEALER | DEALER | Load balancing relay |
| PAIR | PAIR | Inter-thread bridge |

## 3. PUB/SUB Proxy — XSUB/XPUB

The most common proxy pattern.

```mermaid
flowchart LR
    PUB -->|data| XSUB
    XSUB ==>|proxy| XPUB
    XPUB -->|data| SUB
    SUB -.->|subscribe| XPUB
    XPUB -.->|proxy| XSUB
    XSUB -.->|subscribe| PUB
```

### 3.1 Built-in Proxy

```c
void *xsub = zlink_socket(ctx, ZLINK_SOCKET_XSUB);
zlink_bind(xsub, "tcp://*:5556");      /* PUBs connect here */

void *xpub = zlink_socket(ctx, ZLINK_SOCKET_XPUB);
zlink_bind(xpub, "tcp://*:5557");      /* SUBs connect here */

void *capture = zlink_socket(ctx, ZLINK_SOCKET_PUB);
zlink_bind(capture, "tcp://*:5558");   /* optional: message recording */

zlink_proxy(xsub, xpub, capture);      /* blocking */
```

`zlink_proxy()` handles two things internally:
- **Data relay**: Pulls messages from XSUB and pushes to XPUB
- **Subscription propagation**: Pulls subscription events from XPUB and pushes to XSUB

### 3.2 Manual Proxy

When custom logic (logging, filtering, topic transformation) is needed,
build a manual proxy using the public per-socket APIs: `zlink_subscribe_part()`
/ `zlink_publish_part()` for data, `zlink_xpub_recv_part()` /
`zlink_set_subscription()` for subscriptions.

#### Data Flow

| Step | Socket | API | Description |
|------|--------|-----|-------------|
| 1 | XSUB | `zlink_subscribe_part(xsub, ...)` | Receive one payload part (topic returned separately) |
| 2 | App | Custom logic | Filtering, transformation, logging |
| 3 | XPUB | `zlink_publish_part(xpub, topic, ...)` | Publish that part |

#### Subscription Propagation

| Step | Socket | API | Description |
|------|--------|-----|-------------|
| 1 | XPUB | `zlink_xpub_recv_part(xpub, ...)` | Receive SUB subscribe/unsubscribe events |
| 2 | App | Custom logic | Authorization, topic remapping |
| 3 | XSUB | `zlink_set_subscription(xsub, topic)` | Propagate to upstream PUB |

#### Full Code

```c
void *xsub = zlink_socket(ctx, ZLINK_SOCKET_XSUB);
void *xpub = zlink_socket(ctx, ZLINK_SOCKET_XPUB);
zlink_bind(xsub, "tcp://*:5556");
zlink_bind(xpub, "tcp://*:5557");

while (running) {
    /* Data relay: XSUB -> app -> XPUB, one part at a time */
    char topic[256];
    size_t topic_len = 0;
    zlink_msg_t part;
    zlink_part_flag_t more;

    zlink_msg_init(&part);
    zlink_recv_result_t rc = zlink_subscribe_part(
        xsub, NULL, topic, sizeof(topic), &topic_len, &part, &more,
        ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc == ZLINK_RECV_OK) {
        /* Insert custom logic here (filtering, logging, etc.) */
        /* `more` carries ZLINK_PART_MORE / ZLINK_PART_FINAL straight through,
           so a multipart record on XSUB stays one record on XPUB. */
        zlink_publish_part(xpub, topic, &part, ZLINK_SEND_FLAGS_NONE, more);
    }

    /* Subscription propagation: XPUB -> app -> XSUB */
    const zlink_routing_id_t *sub_rid = NULL;
    int subscribed = 0;
    char sub_topic[256];
    size_t sub_len = 0;
    zlink_recv_result_t sub_rc = zlink_xpub_recv_part(
        xpub, &sub_rid, &subscribed, sub_topic, sizeof(sub_topic), &sub_len,
        ZLINK_RECV_FLAGS_DONTWAIT);
    if (sub_rc == ZLINK_RECV_OK) {
        /* Insert custom logic here (authorization, remapping, etc.) */
        if (subscribed)
            zlink_set_subscription(xsub, sub_topic);
        else
            zlink_unset_subscription(xsub, sub_topic);
    }
}
```

### 3.3 Why XSUB/XPUB?

| Question | With SUB/PUB | With XSUB/XPUB |
|----------|-------------|-----------------|
| Data pass-through | SUB local filter on — must subscribe | XSUB local filter off — **passes all** |
| Subscription events | PUB doesn't expose | XPUB exposes them via `zlink_xpub_recv_part()` |
| Proxy suitability | Proxy must manage topics itself | **Relay only — ideal for proxy** |

> **Key point:** `zlink_proxy()` uses the same internal recv/send paths as the
> raw socket APIs, not the public `zlink_send_part()`/`zlink_recv_part()`
> surface. Through that public surface, `zlink_send_part()` on XSUB still
> returns `ZLINK_SUBMIT_NOT_SUPPORTED` and `zlink_recv_part()` on XPUB still
> returns `ZLINK_RECV_NOT_SUPPORTED`. Proxy operation is only possible via
> `zlink_proxy()` or the manual approach above (using the dedicated
> `zlink_subscribe_part()`, `zlink_publish_part()`, etc. APIs).

## 4. Request/Reply Proxy — ROUTER/DEALER

```
Client (DEALER) --> ROUTER == proxy ==> DEALER --> Server (ROUTER)
```

```c
void *frontend = zlink_socket(ctx, ZLINK_SOCKET_ROUTER);
zlink_bind(frontend, "tcp://*:5559");

void *backend = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
zlink_bind(backend, "tcp://*:5560");

zlink_proxy(frontend, backend, NULL);  /* blocking */
```

ROUTER/DEALER proxy has no subscription propagation, so `zlink_proxy()`
alone is sufficient. For manual construction of the ROUTER-facing side, use
`zlink_router_recv_part()` → `zlink_send_part_rid()` (see the
[ROUTER guide](03-4-router.en.md#2-basic-usage) for the full signature and a
worked example).

## 5. Why Use a Proxy?

**Direct (no proxy) -- N x M connections:**

```mermaid
flowchart LR
    P1[PUB 1] --> S1[SUB 1]
    P1 --> S2[SUB 2]
    P2[PUB 2] --> S1
    P2 --> S2
```

> PUB/SUB must know each other's addresses. Connection count = N x M.

**With proxy -- N + M connections:**

```mermaid
flowchart LR
    P1[PUB 1] --> XSUB
    P2[PUB 2] --> XSUB
    subgraph Proxy
        XSUB --> XPUB
    end
    XPUB --> S1[SUB 1]
    XPUB --> S2[SUB 2]
```

> Only the proxy address is needed. Connection count = N + M.

| Use Case | Description |
|----------|-------------|
| **Reduce connections** | N×M → N+M |
| **Address decoupling** | PUB/SUB don't need each other's endpoints |
| **Dynamic scaling** | PUB/SUB add/remove independently |
| **Subscription transformation** | XPUB MANUAL mode for topic remapping/filtering |
| **Network bridging** | Connect different network segments (e.g., inproc ↔ tcp) |
| **Monitoring** | Capture socket records all passing messages |

---
<!-- zlink-nav:bottom:start -->
[← STREAM](03-5-stream.en.md) | [Transport →](04-transports.en.md)
<!-- zlink-nav:bottom:end -->
