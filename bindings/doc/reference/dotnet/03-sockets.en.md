[한국어](03-sockets.ko.md) | English

[Reference index](README.en.md)

# 03. Sockets

This category covers the eight socket-type interfaces (created via the `IContext` factories in
the Core category), their shared lifecycle/option base, and their per-type typed options. Every
socket's `Send`/`Publish`/`Request`/`Reply` returns the operation-builder family documented in the
Messaging category — this category only covers where each builder starts and what each socket
type uniquely adds. The exact signatures are owned by
[`Contracts/Sockets/`](../../../../bindings/dotnet/src/Zlink/Contracts/Sockets/).

---

## `ISocket` / `IConnectableSocket` shared lifecycle

The base contract every socket type implements: binding, TLS, monitoring, disposal, and (for every
socket except the base marker) outbound connection.

```csharp
socket.Bind("tcp://*:5555");
socket.SetTlsServer(certPath, keyPath, requireClientCert: true);
using IZlinkSocket monitor = (IZlinkSocket)socket.MonitorOpen(SocketEvent.All);
socket.Close();
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `Options` | — | `CommonSocketOptions`, below |
| `Bind(string address)` / `Unbind(string address)` | — | listen / stop listening |
| `MonitorOpen(SocketEvent events)` | `SocketEvent.All` | opens an `ISocketMonitor` (Eventing category) |
| `SetTlsServer(certPath, keyPath, requireClientCert)` | `requireClientCert = false` | apply before `Bind` |
| `SetTlsClient(caCertPath, hostname, trustSystem)` | `trustSystem = false` | apply before `Connect` |
| `Close()` | — | closes the native socket immediately |
| `Connect(string)` / `Disconnect(string)` / `DisconnectRid(RoutingId)` | — | `IConnectableSocket` only — every socket type except the base `IZlinkSocket` marker |

**Completion result.** All synchronous, no return value except `MonitorOpen` (returns
`ISocketMonitor`, caller-owned). `Close()` closes immediately, unlike `Dispose()`.
`ISocket`/`IZlinkSocket` are themselves `IDisposable`/`IAsyncDisposable`.

**When to use.** Call `SetTlsServer`/`SetTlsClient` before `Bind`/`Connect` — applying either after
the socket is already bound or connected has no effect. Call `Close()` only when the native socket
must release immediately rather than through normal disposal.

---

## `CommonSocketOptions`

The typed options facade shared by every socket type, reached via `socket.Options`.

```csharp
socket.Options.SendHighWaterMark = 100_000;
socket.Options.Linger = TimeSpan.FromSeconds(1);
socket.Options.SubmitRetryMode = SubmitRetryMode.LocalFailure;
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `MaxMessageSize` (`long`) | -1 (no limit) | maximum size in bytes of a single accepted message |
| `SendHighWaterMark` / `ReceiveHighWaterMark` (`ulong`) | 0 (no limit) | accounted-byte send/receive queue limit — see Core category's byte-HWM note |
| `SendBufferSize` / `ReceiveBufferSize` (`int`) | -1 (OS default) | OS-level socket send/receive buffer size |
| `Linger` (`TimeSpan?`) | null (wait indefinitely) | upper bound on how long `Close`/`Dispose` waits for pending sends to flush |
| `ReconnectInterval` / `ReconnectIntervalMax` (`TimeSpan?`) | null (disables/uncaps) | delay between reconnect attempts, and its cap |
| `Backlog` (`int`) | OS default | pending-connection queue length for a listening socket |
| `ReceiveTimeout` / `SendTimeout` / `ConnectTimeout` / `HandshakeInterval` (`TimeSpan?`) | null (block indefinitely / OS or native default) | upper bound on how long the matching blocking operation waits |
| `TcpKeepAlive` (`int`, -1/0/1) | OS default | OS TCP keepalive mode |
| `IPv6` (`bool`) | `false` | whether the socket accepts IPv6 connections |
| `TcpNoDelay` (`bool`) | `false` | disables Nagle's algorithm when `true` |
| `Immediate` (`bool`) | `false` | whether a send requires a live connection now, instead of queueing until one exists |
| `SubmitRetryMode` | `Off` | whether a failed submit retries automatically on local back-pressure |
| `SubmitRetryTimeoutMilliseconds` / `SubmitRetryAttempts` (`int`) | mode default | retry timeout and attempt cap when `SubmitRetryMode` is `LocalFailure` |
| `RoutingIdDuplicatePolicy` | `Reject` | what happens when a peer reuses an existing routing id |
| `LastEndpoint` | read-only | the concrete resolved bind address |

**Completion result.** Every property get/set is synchronous.

**When to use.** Set `SendHighWaterMark`/`ReceiveHighWaterMark` and `Linger` before the socket
starts exchanging messages when the defaults don't fit the deployment. Read `LastEndpoint` after
binding a wildcard address to learn the resolved port.

---

## `IPairSocket`

A PAIR socket: exclusive one-to-one peering with no routing, no fields or options beyond the
shared `IMessageSocket` surface.

```csharp
using IPairSocket pair = context.CreatePairSocket();
pair.Send().Message(Message.From("ping")).Submit();
using Received received = Received.Create();
if (pair.Recv(received)) { /* ... */ }
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `Send()` | — | starts the shared `SendOperation` builder (Messaging category) |
| `Recv(Received result, RecvFlags flags)` | `RecvFlags.None` | populates `result` |
| `OnSendReady(Action handler)` | — | back-pressure-cleared callback, background dispatch thread |

`IPairSocket` adds nothing beyond this shared `IMessageSocket` surface.

**Completion result.** `Recv` returns `bool` — `false` only under `RecvFlags.DontWait` with
nothing available.

**When to use.** Use PAIR for an exclusive point-to-point link — it has no peer routing and does
not load-balance.

---

## `IDealerSocket`

A DEALER socket: load-balances sends across its connected peers and can issue routed requests.

```csharp
using IDealerSocket dealer = context.CreateDealerSocket();
dealer.SetRoutingId(RoutingId.From("worker-3"));
IReadOnlyList<Message> reply = await dealer.Request()
    .Message(Message.From("payload"))
    .Async();
```

**Options.** Adds to `IMessageSocket`:

| Member | Default | Meaning |
| --- | --- | --- |
| `Options.Probe` | — | `bool`, set-only; sends an empty probe on connect |
| `Options.RequestTimeout` | — | `TimeSpan?`, set-only |
| `Options.PeerWeight` | — | `int` 0-100, load-balancing weight |
| `SetRoutingId(RoutingId)` / `GetRoutingId()` | — | assigns/reads this socket's own routing id, observed by peers on connect |
| `Request()` | — | starts the shared `RequestOperation` builder; no target parameter — DEALER has no API-level peer routing id |

**Completion result.** `Request()`'s builder resolves per the Messaging category's
operation-builder entry. `SetRoutingId`/`GetRoutingId` are synchronous.

**When to use.** Set `SetRoutingId` before connecting so peers observe it from the first message.
DEALER has no protocol envelope helper to reply to an arbitrary token — reply from a received
request's context (`Received.Reply()`, Messaging category) or from an explicit ROUTER/service
reply surface instead.

---

## `IRouterSocket`

A ROUTER socket: routes messages to peers addressed by routing id, and can reply to a specific
peer's request.

```csharp
using IRouterSocket router = context.CreateRouterSocket();
router.Send(peerRid).Message(Message.From("hello")).Submit();
router.OnCompletionControl((rid, parts) => { /* ... */ });
```

**Options.** Adds to `IRoutedMessageSocket` (`Send(RoutingId)`, `Recv(Received, RecvFlags)`,
`OnSendReady(Action)`) and `IConnectableSocket`:

| Member | Default | Meaning |
| --- | --- | --- |
| `Options.Mandatory` | `false` | `bool`; error instead of silent drop on an unknown route |
| `Options.Handover` | `false` | `bool`; shorthand over `RoutingIdDuplicatePolicy` |
| `Options.Probe` | — | `bool` |
| `Options.ConnectRoutingId` / `SetConnectRoutingId(RoutingId)` | read-only getter | assigns the next outbound connection's id instead of letting the peer choose |
| `Options.RequestTimeout` | — | `TimeSpan?` |
| `Options.PeerWeight` | — | `int` 0-100 |
| `SetRoutingId(RoutingId)` / `GetRoutingId()` | — | assigns/reads this socket's own routing id, observed by peers on connect |
| `Request(RoutingId peerRid)` | — | Messaging category's `RequestOperation`, addressed to a specific peer |
| `Reply(RoutingId rid, ulong requestSeq)` | — | Messaging category's `ReplyOperation`, answering that peer's request |
| `TrySendCompletionControl(RoutingId peerRid, IReadOnlyList<Message> parts)` | — | sends an opaque control record to a peer over its existing connection, without consuming `parts` |
| `OnCompletionControl(CompletionControlHandler handler)` | — | registers the callback that receives incoming completion-control records |

**Completion result.** `TrySendCompletionControl` returns `bool` synchronously — `false` means
completion-lane back-pressure; other failures throw `ZlinkSubmitException`. `CompletionControlHandler`
runs on a background dispatch thread and owns every message in `parts` — dispose each exactly once.

**When to use.** `Request(peerRid)`/`Reply(rid, requestSeq)` for ROUTER-initiated or
ROUTER-answered request/reply, where DEALER cannot address a specific peer.
`TrySendCompletionControl`/`OnCompletionControl` for an opaque bounded control record on a peer's
existing connection, independent from application-level receive.

---

## `IPubSocket` / `IXPubSocket`

PUB publishes topic-filtered messages, dropping ones with no matching subscriber; XPUB
additionally surfaces subscriber subscription/unsubscription events.

```csharp
using IPubSocket pub = context.CreatePubSocket();
pub.Publish("prices").Message(Message.From(tick)).Submit();

using IXPubSocket xpub = context.CreateXPubSocket();
using SubscriptionEvent evt = new SubscriptionEvent();
if (xpub.ReceiveSubscriptionEvent(evt)) { /* ... */ }
```

**Options.** Shared `IPublisherSocket`: `Publish(string topic)` (starts the shared
`SendOperation`), `OnSendReady(Action)`. `IPubSocket` also has `SetRoutingId`/`GetRoutingId` —
**`IXPubSocket` does not** (it inherits only `IPublisherSocket`). Both expose `Options` as the
same `PubSocketOptions` facade:

| Member | Default | Meaning |
| --- | --- | --- |
| `Verbose` / `Verboser` | `false` | deliver every (un)subscribe message, including duplicates |
| `Manual` | `false` | subscriptions require `ApproveSubscribe`/`RejectSubscribe` instead of auto-accept |
| `ManualLastValue` | `false` | manual mode that also replays the last cached message per topic to a newly accepted subscriber |
| `NoDrop` | `false` | error instead of silent drop on back-pressure |
| `WelcomeMessage` | none | sent automatically to each newly connected subscriber; getter returns a caller-owned copy |
| `TopicsCount` | read-only | number of distinct topics currently subscribed by any connected peer |
| `ApproveSubscribe(RoutingId)` / `RejectSubscribe(RoutingId)` | — | require `Manual` |
| `ReceiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags)` | `RecvFlags.None` | `IXPubSocket` only |

**Completion result.** `ReceiveSubscriptionEvent` returns `bool` — `false` only under
`RecvFlags.DontWait` with nothing available. `ApproveSubscribe`/`RejectSubscribe` are synchronous,
no return value.

**When to use.** `IXPubSocket` over `IPubSocket` specifically to observe subscriber churn via
`ReceiveSubscriptionEvent`, or manual admission via `Manual`/`ApproveSubscribe`/`RejectSubscribe`;
otherwise the two behave the same for publishing.

---

## `ISubSocket` / `IXSubSocket`

SUB subscribes to topics with subscriptions set as socket options; XSUB carries its subscriptions
as messages instead.

```csharp
using ISubSocket sub = context.CreateSubSocket();
sub.SetSubscription("prices.");
using TopicMessage msg = new TopicMessage();
if (sub.Subscribe(msg)) { /* ... */ }
```

**Options.** Shared `ISubscriberSocket`:

| Member | Default | Meaning |
| --- | --- | --- |
| `SetSubscription(string)` / `UnsetSubscription(string)` | — | adds/removes a topic filter; subscriptions accumulate |
| `SubscriptionAt(int index)` | `SubscriptionEntry?` | the filter at that index, `null` when out of range |
| `Subscribe(TopicMessage result, RecvFlags flags)` | `RecvFlags.None` | populates `result` with the next matching publish |
| `Options.TopicsCount` (`int`) | read-only | number of active subscription filters |
| `SetRoutingId(RoutingId)` / `GetRoutingId()` | — | assigns/reads this socket's own routing id; `ISubSocket` only |

**`IXSubSocket` adds nothing beyond `ISubscriberSocket`** — no `SetRoutingId`/`GetRoutingId`, no
unique members; every operation is the shared surface (its own `Options` is the same
`SubSocketOptions` type).

**Completion result.** `Subscribe` returns `bool` — `false` only under `RecvFlags.DontWait` with
nothing available.

**When to use.** `ISubSocket` for the common case (subscriptions as socket options); `IXSubSocket`
specifically when subscriptions must be carried as ordinary messages instead.

---

## `IStreamSocket`

A STREAM socket: exchanges framed packets directly with raw TCP peers, outside the zlink wire
protocol used by every other socket type.

```csharp
using IStreamSocket stream = context.CreateStreamSocket();
stream.OnPacket((routingId, header, body) => { /* owns header/body; dispose each once */ });
```

**Options.** Extends `IRoutedMessageSocket`:

| Member | Default | Meaning |
| --- | --- | --- |
| `Options.Notify` | `false` | `bool`; deliver peer connect/disconnect as application messages |
| `OnPacket(StreamPacketHandler handler)` | — | background-dispatch-thread callback; handler owns and must dispose `header`/`body` exactly once |
| `RecvPart(out RoutingId? sourceRoutingId, out Message? part, out bool hasMore, RecvFlags flags)` | `RecvFlags.None` | pulls the next packet part; first call fixes this socket to receive mode — cannot combine with `OnPacket` |
| `DisconnectRid(RoutingId peerRid)` | — | disconnects the peer identified by that routing id |

**Completion result.** `RecvPart` returns `bool` synchronously; the returned `part` is
caller-owned. `StreamPacketHandler` transfers message ownership to the callback, which must
dispose `header` and `body` exactly once.

**When to use.** `OnPacket` for a callback-driven packet loop, or `RecvPart` for a pull-based one —
mutually exclusive once the first receive call is made.

---

## Socket enums

Shared enums referenced across every entry above.

| Enum | Used by | Values |
|---|---|---|
| `SocketType` | Internal socket-kind identification | `Any`, `Pair`, `Pub`, `Sub`, `Dealer`, `Router`, `XPub`, `XSub`, `Stream` |
| `AutoHwmProfile` | `IContextOptions.AutoHwmProfile` (Core category) | `Compact`, `LowLatency`, `Balanced`, `Throughput` |
| `RidDuplicatePolicy` | `CommonSocketOptions.RoutingIdDuplicatePolicy`, `RouterSocketOptions.Handover` | `Reject`, `Handover` |
| `SubmitRetryMode` | `CommonSocketOptions.SubmitRetryMode` | `Off`, `LocalFailure` |
| `SendFlags` | Every send/request/reply builder's `.Flags(...)` stage (Messaging category) | `None`, `DontWait` |
| `RecvFlags` | Every `Recv`/`Subscribe`/`ReceiveSubscriptionEvent`/`RecvPart` | `None`, `DontWait` |

**When to use.** `DontWait` on either flags enum turns a blocking call into a non-blocking one that
reports `false`/back-pressure instead of blocking.

---

See [`Contracts/Sockets/`](../../../../bindings/dotnet/src/Zlink/Contracts/Sockets/) and the
[.NET binding spec](../../spec/dotnet/README.en.md) for the full rationale.
