---
title: ".NET Binding Guide"
---

<!-- bindings-nav:start -->
[Guide list](../README.en.md) | [Previous: Overview](../README.en.md) | [Next: C++](../cpp/index.en.md)
<!-- bindings-nav:end -->

# .NET Binding Guide (`Systems.Zlink`)

> **Contract-owning document for this chapter** — the [.NET bindings spec](../../spec/dotnet/README.en.md)
> covers it. This chapter shows that contract as working sample code.

Covers **how to use zlink in .NET (`Systems.Zlink`)** in one chapter — installation,
core types, ownership, error handling, and deployment. For a deep dive into
messaging concepts (socket patterns, services, operations), see the core guide
links under [See Also](#see-also).

---

## Installation

Ships as a single NuGet package, **`Systems.Zlink`**, with the native core bundled.

```bash
dotnet add package Systems.Zlink
```

- **.NET 8.0** or later (`net8.0`).
- No native install step needed — per-RID binaries load automatically.
  (see [Native Library / Deployment](#native-library--deployment))

```csharp
using Systems.Zlink;   // every public API lives in this namespace
```

---

## 5-Minute Example

A minimal example with a `Pair` socket where one side sends `PING` and the other
replies with `ACK`. The server binds, the client connects.

```csharp
// Server
using var ctx = Zlink.CreateContext();
using var server = ctx.CreatePairSocket();
using var mon = server.MonitorOpen(SocketEvent.ConnectionReady);
server.Bind("tcp://127.0.0.1:5555");
mon.Recv();   // wait for connection

using var received = Received.Create();
server.Recv(received);
Console.WriteLine(received.FirstPart().GetString());   // PING

using var reply = Message.From("ACK");
server.Send().Message(reply).Submit();
```

```csharp
// Client
using var ctx = Zlink.CreateContext();
using var client = ctx.CreatePairSocket();
using var mon = client.MonitorOpen(SocketEvent.ConnectionReady);
client.Connect("tcp://127.0.0.1:5555");
mon.Recv();

using var ping = Message.From("PING");
client.Send().Message(ping).Submit();

using var received = Received.Create();
client.Recv(received);
Console.WriteLine(received.FirstPart().GetString());   // ACK
```

---

## Core Types

The 4 fundamental types every feature shares.

### 1. Context

The runtime entry point for a process. Usually you create one and build every
socket/service from it.

```csharp
using var ctx = Zlink.CreateContext();
ctx.Options.IoThreads  = 4;     // number of I/O threads
ctx.Options.MaxSockets = 1024;  // max socket count
// set options before creating sockets.
```

`IContext` is `IDisposable`/`IAsyncDisposable`. `Shutdown()` can interrupt
in-flight operations on close, and `using` disposes it automatically.

### 2. Message

A single payload frame. Built from a string, bytes, or a pre-allocated buffer.

```csharp
byte[] buffer = GetPayload();

using var fromText  = Message.From("payload");      // string (UTF-8)
using var fromBytes = Message.From(buffer);         // copies a byte[] / ReadOnlySpan<byte>
using var sized     = new Message(1024);            // pre-allocate, fill via AsSpan()

int    size = fromText.Size;
string text = fromText.GetString();                  // UTF-8 decode
ReadOnlySpan<byte> view = fromText.AsReadOnlySpan();  // read without copying
byte[] copy             = fromText.ToArray();         // copy out
```

`Message` owns native storage, so it's `IDisposable`. The span returned by
`AsSpan()`/`AsReadOnlySpan()` is only valid while the message is alive. See the
[message API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/) for the
message model concept.

The binding doesn't provide object codec packages such as JSON, Protobuf, or
MessagePack. This layer keeps only a low-level API that exchanges raw `Message`
and byte payloads. If you need object serialization, register a framework codec
extension during the framework's configuration stage. On surfaces that exchange
raw `Message` directly — such as the framework's actor join callback — the
application layer explicitly builds and interprets the byte payload.

### 3. Received

A **reusable envelope** that holds a receive result. Build it once on the hot
path and reuse it across a `Recv(...)` loop to eliminate allocation.

```csharp
using var received = Received.Create();
socket.Recv(received);

Message      first = received.FirstPart();   // first part (no ownership transfer)
string       body  = first.GetString();
RoutingId?   from  = received.RoutingId;     // present if a routing path exists
ulong?       seq   = received.RequestSeq;    // present for request/reply
IReadOnlyList<Message> parts = received.Parts;  // full multipart set
```

### 4. RoutingId

A binary-safe value type identifying a peer, spot, or actor. Built only through
static factories. See [routing ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
for the concept and policy.

```csharp
RoutingId a = RoutingId.From("order-client");       // UTF-8 string
RoutingId b = RoutingId.From(0xC0FFEEu);             // uint32 (big-endian)
RoutingId c = RoutingId.From(Guid.NewGuid());        // 16-byte UUID
RoutingId d = RoutingId.FromHex("0a1b2c");           // raw hex
string    s = a.ToString();                          // display string
string    h = a.ToHex();                             // preserves raw bytes
```

---

## Ownership And Lifetime

`IContext`, sockets, `Message`, and `Received` all wrap native resources and
implement `IDisposable` (and mostly `IAsyncDisposable`). **Whatever you create,
dispose of it** — always with `using` (or `await using`).

- Dispose sockets **before** the context that created them.
- The reply parts returned by `Request().Async()`/`Join(...).Async()`
  (`IReadOnlyList<Message>`) are **owned by the caller** — dispose them after use.
- To hold onto a span, copy it first with `ToArray()`/`CopyTo(...)`.

For thread-safety rules, see [thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/).
`IContext` is safe to share across threads. **Sockets are not** — never call the
same socket from more than one thread concurrently.

---

## Error Handling

Hard failures surface as per-operation typed exceptions. All inherit from
`ZlinkException` and expose `Code` (integer code) and a per-operation `Result`
(enum).

```csharp
try
{
    socket.Bind("tcp://127.0.0.1:5555");
}
catch (ZlinkBindException ex) when (ex.Result == ZlinkBindException.ErrorCode.AddrInUse)
{
    Console.Error.WriteLine("Port already in use.");
}
catch (ZlinkException ex)
{
    Console.Error.WriteLine($"zlink error {ex.Code}: {ex.Message}");
    throw;
}
```

| Exception | Raised by |
|---|---|
| `ZlinkSubmitException` | Send/publish (`Submit`) |
| `ZlinkRequestException` | Request/reply (`Request`) — includes `TimedOut` |
| `ZlinkRecvException` | Receive (`Recv`) |
| `ZlinkBindException` / `ZlinkConnectException` | Bind/connect |
| `ZlinkConfigException` | Option/config |
| `ZlinkCloseException` / `ZlinkHandlerException` | Close/callback |

**No data and transient back-pressure are not exceptions.** Distinguish them by
return value: a non-blocking receive returns `false` from `Recv(...)`, and a
non-blocking send returns `false` from `Submit()`:

```csharp
if (!socket.Recv(received, RecvFlags.DontWait)) { /* no data */ }
if (!socket.Send().Message(m).Flags(SendFlags.DontWait).Submit()) { /* back-pressure */ }
```

---

## C API Mapping

A compressed mapping for anyone coming from the C core (`zlink.h`) or comparing
against another language binding. .NET wraps raw functions in objects and
fluent builders, so this isn't 1:1, but it corresponds at the concept level. See
the [core C API guide](https://kairos-code-dev.github.io/zlink/guide/02-core-api/)
for the full list of C functions.

| Area | C API (`zlink_*`) | .NET |
|------|-------------------|------|
| Context | `zlink_ctx_new` / `zlink_ctx_term` | `Zlink.CreateContext()` / `IContext.Dispose()` |
| Context options | `zlink_ctx_set` / `zlink_ctx_get` | `IContext.Options` (`IoThreads`, `MaxSockets`, …) |
| Socket creation | `zlink_socket(ctx, TYPE)` | `ctx.Create<Type>Socket()` (e.g. `CreatePairSocket()`) |
| Bind / connect | `zlink_bind` / `zlink_connect` | `socket.Bind(...)` / `socket.Connect(...)` |
| Disconnect | `zlink_disconnect` / `zlink_disconnect_rid` | `socket.Disconnect(string)` / `socket.DisconnectRid(RoutingId)` |
| Socket options | `zlink_set_option` / `zlink_get_option` | strongly typed per-socket properties (`socket.Options`) |
| routing id | `zlink_set_routing_id` / `zlink_get_routing_id` | `socket.SetRoutingId(RoutingId)` / `socket.GetRoutingId()` |
| Message creation | `zlink_msg_init` / `_init_size` / `_init_data` | `new Message(size)` / `Message.From(...)` |
| Message access | `zlink_msg_data` / `zlink_msg_size` | `Message.AsReadOnlySpan()` / `Message.Size` |
| Message release | `zlink_msg_close` / `zlink_multipart_close` | `Message.Dispose()` / `Zlink.MultipartClose(parts)` |
| Send | `zlink_send_part` (+`_rid`) | `socket.Send().Message(...).Submit()` |
| Receive | `zlink_recv_part` | `socket.Recv(Received)` |
| Request / reply | `zlink_dealer_request_part` / `zlink_router_reply_part` | `dealer.Request()....Async()` / `router.Reply(...)` |
| Subscribe | `zlink_set_subscription` / `zlink_subscribe_part` | `socket.SetSubscription(...)` / `socket.Subscribe(TopicMessage)` |
| Monitor | `zlink_socket_monitor_open` / `_recv` | `socket.MonitorOpen(...)` / `monitor.Recv()` |
| Poller / timer | `zlink_poller_*` / `zlink_timer_*` | `Zlink.CreatePoller()` / `Zlink.CreateTimer()` |
| Proxy | `zlink_proxy` / `zlink_proxy_steerable` | `Zlink.Proxy(...)` / `Zlink.ProxySteerable(...)` |

> **Naming convention**: C's `snake_case` becomes `PascalCase` in .NET. The C
> `*_part` family (the multipart substrate) is represented in .NET as accumulated
> `.Message(...)` calls on a fluent builder — the public shape follows language
> convention, but the semantic contract is the same.

---

## Native Library / Deployment

`Systems.Zlink` bundles the native core under `runtimes/<rid>/native`, so no
extra setup is needed for a normal build. The `ZLINK_LIBRARY_PATH` environment
variable can override the load path. For **self-contained**/single-file/**Native
AOT** publishing, make sure the target RID's assets are included in the output
(`dotnet publish -r <rid>`).

Threading: `IContext` is thread-safe and shareable across threads. Sockets are
single-thread-owned — see [thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/)
for the full rules.

---

## Samples

`bindings/dotnet/samples/` has runnable examples organized by feature.

| Sample | Covers |
|---|---|
| `PairRecv` | PAIR send/receive |
| `DealerRouterRecv` | DEALER/ROUTER routing |
| `RequestReplyAsync` | Async request/reply |
| `PubSubRecv` | PUB/SUB topics |
| `MonitorRecv` | Socket monitor |
| `StreamRecv`, `StreamPacketCallback` | STREAM + packet callback |

> SPOT/Actor examples are covered by the framework samples, not the core binding —
> see the [Spot](../../../../framework/doc/framework/common/guide/server/06-spot.en.md) ·
> [Actor](../../../../framework/doc/framework/common/guide/server/07-actor-spot.en.md) guides.

Run: `./samples/run_samples.sh` (or `run_samples.ps1`).

---

## See Also

**Socket patterns**
- [Socket pattern overview](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)
  - [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/)
  - [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/)
  - [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/)
  - [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/)
  - [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/)
  - [Proxy](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)

**Services**
- [Framework service overview](../../../../framework/doc/framework/common/guide/server/03-concepts.en.md)
  - [Spot](../../../../framework/doc/framework/common/guide/server/06-spot.en.md)
  - [Actor](../../../../framework/doc/framework/common/guide/server/07-actor-spot.en.md)

**Operations**
- [Socket options](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/)
- [TLS security](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/)
- [Monitoring](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/)
- [Thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/)
- [Message API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/)
- [Routing ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
