---
title: "Rust Binding Guide"
---

<!-- bindings-nav:start -->
[Guide list](../README.en.md) | [Previous: Go](../go/index.en.md)
<!-- bindings-nav:end -->

# Rust Binding Guide (`zlink`)

> **Contract-owning document for this chapter** — the [Rust bindings spec](../../spec/rust/README.en.md)
> covers it. This chapter shows that contract as working sample code.

Explains how to use zlink in Rust through working sample code.
See the [core guide](https://kairos-code-dev.github.io/zlink/guide/01-overview/) for
messaging concepts.

---

## Installation

Add it to `Cargo.toml`.

```toml
[dependencies]
zlink = "11.2"
```

- **Rust 1.85** or later (edition 2024).
- The native core is linked in at build time.

```rust
use zlink::{Context, Message, Received, RecvFlags};
```

---

## 5-Minute Example — PING/ACK

```rust
use zlink::{Context, Message, Received, RecvFlags};

// Server
let ctx = Context::new().unwrap();
let server = ctx.pair_socket().unwrap();
server.bind("tcp://127.0.0.1:5555").unwrap();

let mut received = Received::empty();
server.recv(&mut received, RecvFlags::NONE).unwrap();
println!("{}", received.parts()[0].as_str().unwrap()); // PING

let ack = Message::try_from(b"ACK").unwrap();
server.send().message(ack).submit().unwrap();
```

```rust
// Client
let ctx = Context::new().unwrap();
let client = ctx.pair_socket().unwrap();
client.connect("tcp://127.0.0.1:5555").unwrap();

let ping = Message::try_from(b"PING").unwrap();
client.send().message(ping).submit().unwrap();

let mut received = Received::empty();
client.recv(&mut received, RecvFlags::NONE).unwrap();
println!("{}", received.parts()[0].as_str().unwrap()); // ACK
```

---

## Core Types

### Context

```rust
let ctx = Context::new().expect("context creation failed");
// dropping ctx interrupts blocking operations on child sockets
```

### Message

`Message` owns a single payload frame. Passing it to `send` moves ownership,
and the compiler prevents any further use.

```rust
// build from a byte slice
let msg = Message::try_from(b"payload").unwrap();

// pre-sized empty frame
let mut msg = Message::with_size(256).unwrap();
msg.data_mut().copy_from_slice(&data);

// send — msg is moved here
socket.send().message(msg).submit().unwrap();
// reusing msg is a compile error → ownership safety is enforced by the type system
```

Reading a received message:

```rust
let part = &received.parts()[0];
let bytes: &[u8] = part.as_bytes();
let text: &str = part.as_str().unwrap();   // UTF-8
let size = part.size();
```

### Received — the receive envelope

```rust
let mut received = Received::empty();   // reusable
socket.recv(&mut received, RecvFlags::NONE).unwrap();

let parts = received.parts();                       // &[Message]
let rid: Option<&RoutingId> = received.routing_id(); // ROUTER/SPOT
let seq: Option<u64> = received.request_seq();
```

### Routing ID

```rust
let rid = RoutingId::from(b"server-01");
socket.set_routing_id(&rid).unwrap();
```

---

## Ownership And Lifetime

Rust's ownership system enforces most of this at compile time.

| Situation | Rule |
|------|------|
| `submit()` succeeds | `Message` was already moved — no further handling needed |
| `submit()` fails | returns `Result::Err`, the builder cleans up its internal state |
| `recv()` | receives in place into `&mut Received`, parts released on drop |
| Async request | owns the reply `Vec<Message>`, each `Message` released on drop |

```rust
// error-handling pattern
let msg = Message::try_from(b"data").unwrap();
match socket.send().message(msg).submit() {
    Ok(_) => { /* sent */ }
    Err(e) => eprintln!("send failed: {e}"),
}
```

---

## Error Handling

The Rust binding returns per-operation error types via `Result`.

```rust
match socket.send().message(msg).submit() {
    Ok(_) => {}
    Err(e) => match e.code() {
        zlink::SubmitResult::Backpressured => { /* retry */ }
        zlink::SubmitResult::NotConnected => { /* not connected */ }
        _ => return Err(e.into()),
    },
}
```

Error types: `SubmitError`, `RequestError`, `RecvError`, `BindError`,
`ConnectError`, `ConfigError`, `CloseError`, `HandlerError`.
Each exposes the result code enum via a `code()` method.

---

## C API ↔ Rust Mapping

| C API | Rust API |
|-------|----------|
| `zlink_ctx_new()` | `Context::new()` |
| `zlink_ctx_term()` | `drop(ctx)` |
| `zlink_socket(ctx, type)` | `ctx.pair_socket()`, etc. |
| `zlink_bind(s, ep)` | `socket.bind(ep)` |
| `zlink_connect(s, ep)` | `socket.connect(ep)` |
| `zlink_send_part(...)` | `socket.send().message(m).submit()` |
| `zlink_recv_part(...)` | `socket.recv(&mut received, flags)` |
| `zlink_msg_data(msg)` | `part.as_bytes()` |
| `zlink_routing_id_t` | `RoutingId` |
| `zlink_socket_monitor_open(...)` | `SocketMonitor::open(&socket)` |
| `zlink_poller_new()` | `Poller::new()` |
| `zlink_timer_new()` | `Timer::new()` |

---

## Native Library / Deployment

The native core links in automatically at build time. Checking the runtime
version:

```rust
let (major, minor, patch) = zlink::version();   // (i32, i32, i32) tuple
println!("zlink {major}.{minor}.{patch}");
```

**Threading rules:**

| Item | Rule |
|------|------|
| `Context` | `Sync` — shareable across threads (`Arc<Context>`) |
| Sockets | `Send`, but single-thread use only. No concurrent access |
| `Message::as_bytes()` | valid only while the message lives |

```rust
use std::sync::Arc;
let ctx = Arc::new(Context::new().unwrap());

let ctx2 = ctx.clone();
std::thread::spawn(move || {
    let socket = ctx2.dealer_socket().unwrap();
    // use socket only from this thread
});
```

---

## Samples

Verified samples live under `bindings/rust/samples/`.

| File | Description |
|------|------|
| `pair_recv_sample.rs` | PAIR send/receive |
| `dealer_router_recv_sample.rs` | DEALER/ROUTER send/receive |
| `request_reply_callback_sample.rs` | Callback request/reply |
| `pubsub_recv_sample.rs` | PUB/SUB publish/subscribe |
| `stream_recv_sample.rs` | STREAM raw TCP |
| `stream_packet_callback_sample.rs` | STREAM packet callback |
| `monitor_recv_sample.rs` | Monitor event receive |

> SPOT/Actor examples are covered by the framework samples, not the core
> binding. Rust doesn't have a framework binding yet.

```bash
cd bindings/rust
cargo run --example pair_recv_sample
```

Generating the API reference:

```bash
cd bindings/rust
cargo doc --no-deps --open
```

---

## See Also

**Socket patterns**
- [Socket pattern overview](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)
  — [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/) · [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/) · [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/) · [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/) · [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/) · [Proxy](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)

**Services**
- [Framework service overview](../../../../framework/doc/framework/common/guide/server/03-concepts.en.md)

**Operations**
- [Socket options](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/) · [TLS security](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/) · [Monitoring](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/) · [Thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/) · [Message API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/) · [Routing ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
