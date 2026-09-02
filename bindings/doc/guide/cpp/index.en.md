---
title: "C++ Binding Guide"
---

<!-- bindings-nav:start -->
[Guide list](../README.en.md) | [Previous: .NET](../dotnet/index.en.md) | [Next: Java](../java/index.en.md)
<!-- bindings-nav:end -->

# C++ Binding Guide (`zlink::`)

> **Contract-owning document for this chapter** — the [C++ bindings spec](../../spec/cpp/README.en.md)
> covers it. This chapter shows that contract as working sample code.

A binding that wraps the C core in RAII (not header-only — compiled and linked).
Explains how to use zlink in C++ through working sample code.
See the [core guide](https://zlink-systems.github.io/zlink/guide/01-overview/) for
the deeper messaging concepts.

---

## Installation

The C++ binding ships via CMake.

```cmake
add_subdirectory(bindings/cpp)
target_link_libraries(my_app PRIVATE zlink::cpp)
```

- **C++20** or later (uses coroutines, concepts).
- The native core is linked in alongside it.

```cpp
#include <zlink.hpp>   // the whole public API
```

---

## 5-Minute Example

```cpp
#include <zlink.hpp>

// Server
zlink::context_t ctx;
zlink::pair_socket_t server (ctx);
server.bind ("tcp://127.0.0.1:5555");

zlink::received_t inbound;
server.recv (inbound);
std::printf ("%s\n", inbound.parts ()[0].to_string ().c_str ()); // PING
inbound.close ();

zlink::message_t ack = zlink::message_t::from ("ACK");
server.send ().message (ack).submit ();
```

```cpp
// Client
zlink::context_t ctx;
zlink::pair_socket_t client (ctx);
client.connect ("tcp://127.0.0.1:5555");

zlink::message_t ping = zlink::message_t::from ("PING");
client.send ().message (ping).submit ();

zlink::received_t inbound;
client.recv (inbound);
std::printf ("%s\n", inbound.parts ()[0].to_string ().c_str ()); // ACK
inbound.close ();
```

---

## Core Types

### Context

`context_t` is RAII-managed. It shuts down automatically in the destructor.

```cpp
{
    zlink::context_t ctx;
    zlink::pair_socket_t socket (ctx);
    // ...
} // destroying ctx interrupts blocking operations on child sockets
```

### Message

`message_t` owns a single payload frame. Passing it to `send` moves ownership,
and it's invalid to use afterward.

```cpp
// build from a string
zlink::message_t msg = zlink::message_t::from ("payload");

// build from bytes
std::vector<uint8_t> bytes = {0x01, 0x02};
zlink::message_t msg = zlink::message_t::from (bytes);

// pre-sized empty frame
zlink::message_t msg = zlink::message_t::allocate (256);
std::memcpy (msg.data (), src, 256);

// send — msg is moved here
socket.send ().message (msg).submit ();
// msg is invalid after send — don't reuse it
```

HWM-managed sends provide both synchronous and asynchronous terminals. On a
plain thread, use `submit()`, which follows Core's blocking admission path. In a
coroutine, `co_await` the `async()` terminal, which uses DONTWAIT and completes
from the socket completion queue.

```cpp
socket.send ().message (msg).submit (); // sync; blocks until HWM admission by default
co_await socket.send ().message (msg).async (); // async; does not block the caller thread
```

Request uses the same two terminal styles: `submit()` blocks until the reply,
while `async()` returns an awaitable completed from the socket completion queue.
The reply is returned by that terminal; it is not DATA received separately.

Core owns retry for an accepted pre-admission operation. Do not add a caller
retry queue or resubmit the same payload. The shared native
`ZLINK_OPT_PENDING_MAX_MSGS/BYTES` limits apply to pending SEND and REQUEST;
there are no send-only pending option names. Completion means local admission,
not peer delivery or an application acknowledgement.

Destroying an unconsumed `async_result_t` detaches only the caller waiter. Before
Core submit, abandon the language operation without calling Core; after a
successful submit, Core may still admit it and the socket owner drains its late
completion. STREAM must select `stream_recv_mode_t::raw` or `packet` before
bind/connect and then use `recv()` or `recv_packet()` respectively.

If a public poller owns `poll_event_t::completion` for a socket, keep another
thread calling `wait()` while any blocking request or awaitable is outstanding.
The wait drains native completions and settles or cleans their binding state;
calling a blocking terminal between waits on that same thread can stall it.

Reading a received message:

```cpp
const zlink::message_t &part = inbound.parts ()[0];
std::string text = part.to_string ();              // copies into a string
std::span<const std::byte> bytes = part.bytes ();  // view (valid only while the message lives)
size_t size = part.size ();
```

### received_t — the receive envelope

```cpp
zlink::received_t inbound;
int rc = socket.recv (inbound);   // 0 = success
// or with flags
socket.recv (inbound, zlink::recv_flags_t::none);

auto parts = inbound.parts ();                              // const vector
auto rid = inbound.routing_id ();                          // optional<routing_id_t>
auto token = inbound.reply_token ();                       // optional<reply_token_t>

inbound.close ();   // explicit release (or via destructor)
```

### Routing ID

```cpp
auto rid = zlink::routing_id_t::from (
    reinterpret_cast<const uint8_t*> (text.data ()), text.size ());
socket.set_routing_id (rid);
```

---

## Ownership And Lifetime

| Situation | Rule |
|------|------|
| `submit()` succeeds | `message_t` is moved — invalid to use afterward |
| `async()` | operation owns the moved message while Core completion is pending |
| `submit()` other failure | throws (`submit_error_t`), message ownership retained |
| `recv()` | receives in place into `received_t&`, released via `close()` or destructor |
| Async request | owns the reply `std::vector<message_t>`, auto-released when the vector is destroyed |

```cpp
try {
    zlink::message_t msg = zlink::message_t::from ("data");
    socket.send ().message (msg).submit ();  // msg is moved on success
} catch (const zlink::submit_error_t &e) {
    // handle send failure
}
```

---

## Error Handling

The C++ binding throws per-operation exceptions that inherit from
`zlink::binding_error_t`.

```cpp
try {
    zlink::message_t msg = zlink::message_t::from ("data");
    socket.send ().message (msg).submit ();
} catch (const zlink::submit_error_t &e) {
    // includes a blocking admission timeout/back-pressure result
    // check e.result() before applying application policy
}
```

Exception types:

| Exception | Raised when | `result()` type |
|------|----------|---------------|
| `submit_error_t` | send/publish failure | `submit_result_t` |
| `request_error_t` | request failure | `request_result_t` |
| `recv_error_t` | receive failure | `recv_result_t` |
| `bind_error_t` | bind failure | `bind_result_t` |
| `connect_error_t` | connect failure | `connect_result_t` |
| `config_error_t` | option-set failure | `config_result_t` |
| `close_error_t` | close failure | `close_result_t` |
| `handler_error_t` | handler registration failure | `handler_result_t` |

All inherit from `binding_error_t` and expose `code()`, `internal_errno()` to
check the native code. Some recv APIs return a `recv_result_t` integer code
instead of throwing (see the samples).

---

## C API Mapping

| C API | C++ API |
|-------|---------|
| `zlink_ctx_new()` | `zlink::context_t{}` |
| `zlink_ctx_term()` | destructor, or `ctx.term()` |
| `zlink_socket(ctx, type)` | `zlink::pair_socket_t{ctx}`, etc. |
| `zlink_bind(s, ep)` | `socket.bind(ep)` |
| `zlink_connect(s, ep)` | `socket.connect(ep)` |
| blocking `zlink_send_part(...)` / `zlink_send_part_rid(...)` | `socket.send().message(m).submit()` |
| DONTWAIT send + completion pull | `co_await socket.send().message(m).async()` |
| `zlink_recv_part(...)` | `socket.recv(received)` |
| `zlink_msg_data(msg)` | `part.data()` / `part.bytes()` |
| `zlink_msg_size(msg)` | `part.size()` |
| `zlink_routing_id_t` | `zlink::routing_id_t` |
| `zlink_socket_monitor_open(...)` | `socket.monitor_open(...)` |
| `zlink_poller_new()` | `zlink::poller_t{}` |
| `zlink_timer_new()` | `zlink::timer_t{}` |

---

## Native Library / Deployment

```cpp
int major, minor, patch;
zlink::version (major, minor, patch);
std::printf ("zlink %d.%d.%d\n", major, minor, patch);

if (zlink::has ("draft")) {
    // draft API supported
}
```

Threading rules:

| Item | Rule |
|------|------|
| `context_t` | shareable across threads |
| Sockets | **single-thread use only**. No concurrent access |
| Completion and receive delivery | observed by caller-owned terminals or pull loops |
| `message_t::bytes()` | span valid only while the message lives |

A synchronous `submit()` without a flag stops its calling thread while waiting
for HWM admission. This only parks that plain thread. In a coroutine that must
keep making progress, use `co_await async()`. Managed send does not expose a
public DONTWAIT flag terminal.

```cpp
// correct pattern: one socket per thread
std::thread worker ([&ctx] {
    zlink::dealer_socket_t socket (ctx);
    socket.connect ("tcp://...");
    // use socket only from this thread
});
```

---

## Samples

Verified samples live under `bindings/cpp/samples/`.

| File | Description |
|------|------|
| `pair_recv_sample.cpp` | PAIR send/receive |
| `dealer_router_recv_sample.cpp` | DEALER/ROUTER send/receive (request/reply) |
| `pubsub_recv_sample.cpp` | XPUB/SUB publish/subscribe |
| `stream_recv_sample.cpp` | STREAM raw TCP |
| `stream_packet_pull_sample.cpp` | STREAM PACKET pull |
| `monitor_recv_sample.cpp` | Monitor event receive |
| `request_reply_async_sample.cpp` | ROUTER/DEALER async request/reply |

> SPOT/Actor examples are covered by the framework C++ samples, not the core
> binding — see [Bingo](https://github.com/zlink-systems/zlink/tree/main/framework/languages/cpp/samples/Bingo)
> etc. under `framework/languages/cpp/samples/`.

```bash
cd bindings/cpp
# samples build only when ZLINK_CPP_BUILD_SAMPLES=ON
cmake -B build -DZLINK_CPP_BUILD_SAMPLES=ON && cmake --build build
./build/sample_cpp_pair_recv_sample
# or run all at once: ./samples/run_samples.sh
```

---

## See Also

- Socket patterns: [overview](https://zlink-systems.github.io/zlink/guide/03-0-socket-patterns/) — [PAIR](https://zlink-systems.github.io/zlink/guide/03-1-pair/) · [PUB/SUB](https://zlink-systems.github.io/zlink/guide/03-2-pubsub/) · [DEALER](https://zlink-systems.github.io/zlink/guide/03-3-dealer/) · [ROUTER](https://zlink-systems.github.io/zlink/guide/03-4-router/) · [STREAM](https://zlink-systems.github.io/zlink/guide/03-5-stream/) · [Proxy](https://zlink-systems.github.io/zlink/guide/03-6-proxy/)
- Operations: [Socket options](https://zlink-systems.github.io/zlink/guide/12-socket-options/) · [TLS](https://zlink-systems.github.io/zlink/guide/05-tls-security/) · [Monitoring](https://zlink-systems.github.io/zlink/guide/06-monitoring/) · [Thread safety](https://zlink-systems.github.io/zlink/guide/11-thread-safety/) · [Message API](https://zlink-systems.github.io/zlink/guide/09-message-api/) · [Routing ID](https://zlink-systems.github.io/zlink/guide/08-routing-id/)
