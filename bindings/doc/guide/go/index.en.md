---
title: "Go Binding Guide"
---

<!-- bindings-nav:start -->
[Guide list](../README.en.md) | [Previous: Python](../python/index.en.md) | [Next: Rust](../rust/index.en.md)
<!-- bindings-nav:end -->

# Go Binding Guide (`zlink.systems/zlink/v11`)

> **Contract-owning document for this chapter** — the [Go bindings spec](../../spec/go/README.en.md)
> covers it. This chapter shows that contract as working sample code.

Explains how to use zlink in Go through working sample code.
The deep explanation of messaging concepts is owned by the
[core guide](https://kairos-code-dev.github.io/zlink/guide/01-overview/);
this guide focuses on the Go API surface.

---

## Installation

Ships as the **`zlink.systems/zlink/v11`** module. The native core is bundled
per platform.

```bash
go get zlink.systems/zlink/v11
```

- **Go 1.25** or later.
- No separate native install needed — per-RID `.so`/`.dll` files load
  automatically.

```go
import zlink "zlink.systems/zlink/v11"
```

---

## 5-Minute Example — PING/ACK

A minimal example with a `Pair` socket where one side sends `PING` and the
other replies with `ACK`. The server binds, the client connects.

```go
// Server
ctx, _ := zlink.NewContext()
defer ctx.Close()

server, _ := ctx.PairSocket()
defer server.Close()

server.Bind("tcp://127.0.0.1:5555")

var received zlink.Received
if _, err := server.Recv(&received, zlink.RecvFlagsNone); err != nil { ... }
defer received.Close()

part, _ := received.SinglePartOrError()
fmt.Println(string(part.Data())) // PING

reply, _ := zlink.NewMessage([]byte("ACK"))
server.Send().Message(reply).Submit(nil)
```

```go
// Client
ctx, _ := zlink.NewContext()
defer ctx.Close()

client, _ := ctx.PairSocket()
defer client.Close()
client.Connect("tcp://127.0.0.1:5555")

ping, _ := zlink.NewMessage([]byte("PING"))
client.Send().Message(ping).Submit(nil)

var received zlink.Received
if _, err := client.Recv(&received, zlink.RecvFlagsNone); err != nil { ... }
defer received.Close()

part, _ := received.SinglePartOrError()
fmt.Println(string(part.Data())) // ACK
```

In real code always check the error. The example above uses `_` just to keep
the flow readable.

---

## Core Types

The 4 fundamental types every feature shares.

### 1. Context

The runtime entry point for a process. Usually you create one and build every
socket/service from it.

```go
ctx, err := zlink.NewContext()
if err != nil { ... }
defer ctx.Close() // closing the context shuts down every child socket
```

Use `Options()` to adjust the I/O thread count:

```go
opts := ctx.Options()
opts.SetIOThreads(4)
```

> It's recommended to close sockets explicitly **before** the context closes.
> Closing the context interrupts blocking operations on any socket still open.
> (see [thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/))

### 2. Message

Owns a single payload frame. Sending transfers ownership, so `Close()` doesn't
need to be called separately. If the send fails, ownership is retained, so
retry or close it explicitly.

```go
// build a copy from a byte slice
msg, err := zlink.NewMessage([]byte("payload"))

// allocate a sized empty frame, fill it directly
msg, err := zlink.NewMessageWithSize(256)
copy(msg.Data(), myData)

// build a copy from a string
msg, err := zlink.NewMessageString("payload")

// discard without sending
defer msg.Close()
```

Use `Data()` to read a received payload. The returned slice is tied to the
message's lifetime, so copy it if you need to keep it:

```go
data := msg.Data()            // valid only while the message is alive
snapshot := msg.Bytes()       // an independent copy
text := msg.Text()            // UTF-8 string conversion
```

### 3. Received — the receive envelope

Holds a received message envelope. Carries a routing ID, part list, and an
optional reply context. Can be declared once and reused across multiple
receives. Parts are released by calling `Close()`.

```go
var received zlink.Received   // stack-declared, no heap allocation
_, err = socket.Recv(&received, zlink.RecvFlagsNone)
defer received.Close()        // releases the received parts

// single-part access
part, err := received.SinglePartOrError()
payload := part.Data()

// multipart access
for _, part := range received.Parts() {
    _ = part.Data()
}

// routing ID (present on ROUTER/SPOT receive)
rid := received.RoutingID() // *RoutingID, nil if absent
```

### 4. RoutingID

An immutable value of 1-255 bytes identifying a peer or spot. Build it directly
or pull it from a receive envelope.

```go
rid := zlink.NewRoutingID([]byte("server-01"))
rid := zlink.NewRoutingIDString("server-01")
rid := zlink.NewRoutingIDUint32(1)          // 4 bytes, big-endian
rid := zlink.NewRoutingIDUUIDBytes(uuid)    // 16-byte UUID
rid, err := zlink.NewRoutingIDFromHex("0102...")   // parsed from hex

fmt.Println(rid.String())  // human-readable form
```

---

## Ownership And Lifetime

The Go binding's ownership rules are simple.

| Situation | Rule |
|------|------|
| `Submit` succeeds | ownership of the added `*Message` transfers to the send stack. No `Close()` needed |
| `Submit` fails | ownership returns to the caller. `Close()` required |
| `Recv` succeeds | the caller owns the `Received`. `defer received.Close()` required |
| `Request.SubmitAsync` completes | ownership of the reply parts (`[]*Message`) comes to the caller. `Close()` each part |
| `Context.Close()` | interrupts every blocking operation under the context |

```go
// pattern: safe even on error
msg, _ := zlink.NewMessage([]byte("data"))
if _, err := socket.Send().Message(msg).Submit(nil); err != nil {
    defer msg.Close() // only closed on send failure
}
// on send success msg is already consumed, no Close() needed
```

---

## Error Handling

The Go binding returns the standard `error` interface. If you need the result
code, check it with a type assertion.

```go
_, err := socket.Send().Message(msg).Submit(nil)
if err != nil {
    var submitErr *zlink.SubmitError
    if errors.As(err, &submitErr) {
        switch submitErr.Result {
        case zlink.SubmitBackpressured:
            // back-pressure — retry shortly
        case zlink.SubmitNotConnected:
            // no connected peer
        default:
            return err
        }
    }
    return err
}
```

Error types:

| Type | Description | Result field |
|------|------|-------------|
| `*SubmitError` | send/publish failure | `SubmitResult` |
| `*RequestError` | request failure | `RequestResult` |
| `*RecvError` | receive failure | `RecvResult` |
| `*BindError` | bind failure | `BindResult` |
| `*ConnectError` | connect failure | `ConnectResult` |
| `*ConfigError` | option-set failure | `ConfigResult` |
| `*CloseError` | close failure | `CloseResult` |
| `*HandlerError` | handler registration failure | `HandlerResult` |

No message on a non-blocking receive isn't an error:

```go
ok, err := socket.Recv(&received, zlink.RecvFlagsDontWait)
if err != nil { /* a real error */ }
if !ok { /* no message */ }
```

---

## C API Mapping

| C API | Go API |
|-------|--------|
| `zlink_ctx_new()` | `zlink.NewContext()` |
| `zlink_ctx_term()` | `ctx.Close()` |
| `zlink_socket(ctx, type)` | `ctx.PairSocket()`, etc. |
| `zlink_close(socket)` | `socket.Close()` |
| `zlink_bind(socket, ep)` | `socket.Bind(ep)` |
| `zlink_connect(socket, ep)` | `socket.Connect(ep)` |
| `zlink_send_part(...)` | `socket.Send().Message(m).Submit(nil)` |
| `zlink_recv_part(...)` | `socket.Recv(&received, flags)` |
| `zlink_msg_data(msg)` | `msg.Data()` |
| `zlink_msg_size(msg)` | `msg.Size()` |
| `zlink_msg_close(msg)` | `msg.Close()` |
| `zlink_routing_id_t` | `zlink.RoutingID` |
| `zlink_socket_monitor_open(...)` | `zlink.OpenSocketMonitor(socket, ...)` |
| `zlink_poller_new()` | `zlink.NewPoller()` |
| `zlink_timer_new()` | `zlink.NewTimer()` |

---

## Native Library / Deployment

The Go binding embeds a per-platform `.so` (Linux) or `.dylib` (macOS). No
separate install — just `go get`.

Checking the native version in use:

```go
v := zlink.RuntimeVersion()
fmt.Printf("zlink %d.%d.%d\n", v.Major, v.Minor, v.Patch)
```

Checking whether a feature is supported:

```go
if zlink.Has("draft") {
    fmt.Println("draft API supported")
}
```

Threading: `Context` can be shared across goroutines, but sockets must be used
**from a single goroutine only**. (see [thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/))

---

## Samples

Verified sample code lives under `bindings/go/samples/`.

| Sample | Description |
|------|------|
| `pair_recv_sample` | PAIR socket send/receive |
| `dealer_router_recv_sample` | DEALER/ROUTER send/receive |
| `request_reply_async_sample` | Async request/reply |
| `pubsub_recv_sample` | XPUB/SUB publish/subscribe |
| `stream_recv_sample` | STREAM raw TCP |
| `stream_packet_callback_sample` | STREAM packet callback |
| `monitor_recv_sample` | Monitor event receive |

> SPOT/Actor examples are covered by the framework samples, not the core
> binding. Go doesn't have a framework binding yet.

Running the samples:

```bash
cd bindings/go
go run ./samples/pair_recv_sample/...
# or run all at once
./samples/run_samples.sh
```

---

## See Also

- **Socket patterns**: [overview](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/) — [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/) · [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/) · [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/) · [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/) · [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/) · [Proxy](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)
- **Operations**: [Socket options](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/) · [TLS](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/) · [Monitoring](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/) · [Thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/) · [Message API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/) · [Routing ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
