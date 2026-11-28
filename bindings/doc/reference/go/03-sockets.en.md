[한국어](03-sockets.ko.md) | English

[Reference index](README.en.md)

# 03. Sockets

The Go binding exposes eight socket types. Use the [Core reference](01-core.en.md) for socket
creation and the [Messaging reference](02-messaging.en.md) for operation builders.
In the following examples, `ctx` is a `*zlink.Context` and `callCtx` is a Go `context.Context`.

---

## Shared socket surface (via embedding, not an interface)

**Options.** Every socket type except `StreamSocket` gets this set for free through embedding.
**Only the combined TLS setters exist**; unlike rust, there are no individual
`SetTLSCert`/`SetTLSKey`/`SetTLSCA`/`SetTLSHostname` methods in this binding.

| Member | Meaning |
| --- | --- |
| `Bind(endpoint string) error` / `Unbind(endpoint string) error` | starts/stops listening on an address |
| `Connect(endpoint string) error` / `Disconnect(endpoint string) error` | connects/disconnects to a peer address |
| `DisconnectRID(peerRID RoutingID) error` | disconnects the peer identified by that routing id |
| `Close() error` | closes the native socket |
| `CommonOptions() *CommonSocketOptions` | the shared options facade |
| `LastEndpoint() (string, error)` | the concrete resolved bind address |
| `SetTLSServer(certPath, keyPath string, requireClientCert bool) error` | combined server-side TLS setup, apply before `Bind` |
| `SetTLSClient(caCertPath, hostname string, trustSystem bool) error` | combined client-side TLS setup, apply before `Connect` |

Every socket supports `Bind`, `Unbind`, `DisconnectRID`, `Close`, `LastEndpoint`, and both TLS
setters. The seven socket types with outbound support also expose `Connect`, `Disconnect`,
`ConnectionAttemptsSnapshot`, and `CommonOptions`. STREAM exposes common options directly
through `SetSendHighWaterMark`/`SendHighWaterMark`,
`SetReceiveHighWaterMark`/`ReceiveHighWaterMark`, `SetLinger`, `SetReceiveTimeout`,
`SetSendTimeout`, `SetTCPKeepalive`, `SetTCPNoDelay`, `SetIPv6`, and `SetReceiveFlowState`.

**Completion result.** Lifecycle methods and TLS setters return `error`. `LastEndpoint()`
returns `(string, error)`. `CommonOptions()` returns the options facade directly.

**When to use.** Use STREAM to accept inbound connections from raw TCP peers. Choose a
socket type with outbound support if you need `Connect` or the full `CommonOptions()`
facade. See [Connection attempt snapshots](04-eventing.en.md#connection-attempt-snapshots)
for outbound connection readiness.

---

## `CommonSocketOptions` / `PubSocketOptions`

The typed options facades — `CommonSocketOptions` shared by every socket type that embeds
`connectionSocket` (reached via `.CommonOptions()`), `PubSocketOptions` specific to PUB/XPUB
(reached via `.PubOptions()`).

```go
opts := dealer.CommonOptions()
opts.SetSendHighWaterMark(100_000)
opts.SetLinger(time.Second)
opts.SetSubmitRetryMode(zlink.SubmitRetryLocalFailure)
```

**Options — `CommonSocketOptions`.** Every accessor returns `(T, error)`/`error`.

| Member | Meaning |
| --- | --- |
| `Linger()` / `SetLinger(time.Duration)` | upper bound on how long `Close()` waits for pending sends to flush |
| `SendHighWaterMark()` / `SetSendHighWaterMark(uint64)` | outbound accounted-byte HWM |
| `ReceiveHighWaterMark()` / `SetReceiveHighWaterMark(uint64)` | inbound accounted-byte HWM |
| `SendTimeout()` / `SetSendTimeout(time.Duration)` | upper bound on how long a blocking send waits |
| `ReceiveTimeout()` / `SetReceiveTimeout(time.Duration)` | upper bound on how long a blocking receive waits |
| `Immediate()` / `SetImmediate(bool)` | whether a send requires a live connection now, instead of queueing until one exists |
| `RidDuplicatePolicy()` / `SetRidDuplicatePolicy(RidDuplicatePolicy)` | what happens when a peer reuses an existing routing id |
| `ConnectTimeout()` / `SetConnectTimeout(time.Duration)` | upper bound on how long connect handshake waits |
| `IPv6()` / `SetIPv6(bool)` | whether the socket accepts IPv6 connections |
| `TCPNoDelay()` / `SetTCPNoDelay(bool)` | disables Nagle's algorithm when `true` |
| `TCPKeepalive()` / `SetTCPKeepalive(bool)` | OS TCP keepalive mode |
| `MaxMessageSize()` / `SetMaxMessageSize(int64)` | maximum size in bytes of a single accepted message |
| `Backlog()` / `SetBacklog(int)` | pending-connection queue length for a listening socket |
| `ReconnectInterval()` / `SetReconnectInterval(time.Duration)` | delay between reconnect attempts |
| `ReconnectIntervalMax()` / `SetReconnectIntervalMax(time.Duration)` | cap on the reconnect delay |
| `SubmitRetryMode()` / `SetSubmitRetryMode(SubmitRetryMode)` | whether a failed submit retries automatically on local back-pressure |
| `SubmitRetryTimeout()` / `SetSubmitRetryTimeout(time.Duration)` | retry timeout when `SubmitRetryMode()` is `SubmitRetryLocalFailure` |
| `SubmitRetryAttempts()` / `SetSubmitRetryAttempts(int)` | retry attempt cap when `SubmitRetryMode()` is `SubmitRetryLocalFailure` |
| `LastEndpoint()` | read-only, delegates to the socket directly — the concrete resolved bind address |

**Options — `PubSocketOptions`.**

| Member | Meaning |
| --- | --- |
| `NoDrop()` / `SetNoDrop(bool)` | error instead of silent drop on back-pressure |
| `Verbose()` / `SetVerbose(bool)` / `Verboser()` / `SetVerboser(bool)` | deliver every (un)subscribe message, including duplicates |
| `Manual()` / `SetManual(bool)` | subscriptions require `ApproveSubscribe`/`RejectSubscribe` instead of auto-accept |
| `ManualLastValue()` / `SetManualLastValue(bool)` | manual mode that also replays the last cached message per topic to a newly accepted subscriber |
| `TopicsCount() (int, error)` | read-only, active subscription count |
| `WelcomeMessage()` / `SetWelcomeMessage(*Message)` | sent automatically to each newly connected subscriber |
| `ApproveSubscribe(RoutingID) error` / `RejectSubscribe(RoutingID) error` | set-only — no corresponding getters |

**Completion result.** Every accessor is synchronous, returning `error` alongside its value —
consistent with the Core category's `Context.Options()` convention.

**When to use.** Set `SendHighWaterMark`/`ReceiveHighWaterMark` and `Linger` before the socket
starts exchanging messages when the defaults don't fit the deployment. Reach `PubSocketOptions`
through `PubSocket`/`XPubSocket` directly (both embed `publishSocket`, which declares
`PubOptions()`) rather than through `CommonOptions()`.

---

## `PairSocket`

An exclusive one-to-one peering socket with no routing.

```go
pair, err := ctx.PairSocket()
pair.Send().Message(ping).Submit(callCtx)
var received zlink.Received
ok, err := pair.Recv(&received, zlink.RecvFlagsNone)
```

**Options.** Plus the shared lifecycle/TLS/options surface above.

| Member | Meaning |
| --- | --- |
| `Send() SendOp` | starts the shared send builder |
| `Recv(out *Received, flags RecvFlags) (bool, error)` | populates `out` with the next message |
| Send `Submit(ctx)` | Returns `(SendSubmission, error)`. Call `Admitted(ctx)` to wait for admission while Core handles pre-admission retry. |

**Completion result.** `Recv` returns `(false, nil)` only when `RecvFlagsDontWait` is set and no
message is available.

**When to use.** Use PAIR for an exclusive point-to-point link — it has no peer routing and does
not load-balance.

---

## `DealerSocket`

Load-balances sends across its connected peers and can issue routed requests.

```go
dealer, err := ctx.DealerSocket()
dealer.SetRoutingID(zlink.NewRoutingIDString("worker-3"))
request, err := dealer.Request().Message(payload).Submit(callCtx)
if err != nil {
    return err
}
parts, err := request.Reply(callCtx)
if err != nil {
    return err
}
defer zlink.MultipartClose(parts)
```

**Options.** Same shared surface as `PairSocket`, plus:

| Member | Meaning |
| --- | --- |
| `Send() SendOp` | starts the shared send builder |
| `Recv(out, flags) (bool, error)` | populates `out` with the next message |
| `Request() RequestOp` | starts the shared request builder; no target parameter — DEALER has no API-level peer routing id |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | assigns/reads this socket's own routing id, observed by peers on connect |
| `SetProbe(bool) error` / `Probe() (bool, error)` | Controls whether the socket sends an empty probe on connect. |
| `Weight()` / `SetWeight(int)` | load-balancing weight, both directions |
| `SetRequestTimeout(time.Duration) error` / `RequestTimeout() (time.Duration, error)` | Sets or reads the request timeout. |

**Completion result.** `Recv` follows the same `(false, nil)`-on-`DontWait` convention as
`PairSocket`.

**When to use.** Set `SetRoutingID` before connecting so peers observe it from the first message.
DEALER has no protocol envelope helper to reply to an arbitrary token — reply from a received
request's context (`Received.Reply()`) or use ROUTER's explicit reply surface instead.

---

## `RouterSocket`

Routes messages to peers addressed by routing id and replies to a specific peer's request.

```go
router, err := ctx.RouterSocket()
router.SendTo(peerRID).Message(hello).Submit(callCtx)
router.Reply(peerRID, replyToken).Message(reply).Submit(callCtx)
```

**Options.** Same shared surface as `PairSocket`, plus:

| Member | Meaning |
| --- | --- |
| `SendTo(target RoutingID) SendOp` | starts the shared send builder, addressed to that peer |
| `Recv(out, flags) (bool, error)` | populates `out` with the next application message |
| `Request(peerRID RoutingID) RequestOp` | Messaging category's `RequestOp`, addressed to a specific peer |
| `Reply(rid RoutingID, token ReplyToken) ReplyOp` | Messaging category's `ReplyOp`, answering the request identified by the opaque token |
| `Received.ReplyToken()` | returns the socket-bound token supplied by ROUTER receive |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | assigns/reads this socket's own routing id, observed by peers on connect |
| `SetMandatory(bool) error` / `Mandatory() (bool, error)` | Controls whether an unknown route causes a send error. |
| `SetProbe(bool) error` / `Probe() (bool, error)` | Controls whether the socket sends an empty probe on connect. |
| `SetHandover(bool) error` / `Handover() (bool, error)` | Reads or sets the shared duplicate policy. Maps `true` to `RidDuplicateHandover` and `false` to `RidDuplicateReject`. |
| `SetConnectRoutingID(RoutingID) error` | Sets the local alias for the next outbound connection. |
| `Weight()` / `SetWeight(int)` | load-balancing weight, both directions |
| `SetRequestTimeout(time.Duration) error` / `RequestTimeout() (time.Duration, error)` | Sets or reads the request timeout. |
| request `Submit(ctx)` | returns a submission. Call `Reply(ctx)` for reply parts or a terminal error |
| send `Submit(ctx)` | returns a submission. Call `Admitted(ctx)` to wait for admission |

**Completion result.** `Recv` follows the same convention as `PairSocket`.

**When to use.** Use `Request(peerRID)`/`Reply(rid, token)` for ROUTER-initiated or
ROUTER-answered request/reply, where DEALER cannot address a specific peer. The token comes from
receive and must not be synthesized or reused.

---

## `PubSocket` / `SubSocket` / `XPubSocket` / `XSubSocket`

PUB publishes topic-filtered messages, dropping ones with no matching subscriber; SUB subscribes
with subscriptions set as socket options; XPUB adds subscriber-event surfacing, XSUB carries
subscriptions as messages instead. `PubSocket`/`XPubSocket` both embed `publishSocket`; `SubSocket`/
`XSubSocket` both embed `subscribeSocket`.

```go
pubSocket, err := ctx.PubSocket()
pubSocket.Publish("prices").Message(tick).Submit(callCtx)

sub, err := ctx.SubSocket()
sub.SetSubscription("prices.")
var msg zlink.TopicMessage
ok, err := sub.Subscribe(&msg, zlink.RecvFlagsNone)
```

**Options.** **Neither `PubSocket` nor `XPubSocket` declares `SetRoutingID`/`RoutingID`** — no
routing-id surface at all on either type in this binding, the same as every other language covered
so far. Subscription receive remains a pull-only path.

| Type | Member | Meaning |
| --- | --- | --- |
| `PubSocket` | `Publish(topic string) PublishOp` | starts the publish builder |
| | `PublishSubmitOp.Submit(ctx) (bool, error)` | synchronous lossy/NODROP publish terminal |
| | `PubOptions() *PubSocketOptions` | the per-type options facade |
| `SubSocket` / `XSubSocket` | `Subscribe(out *TopicMessage, flags RecvFlags) (bool, error)` | populates `out` with the next matching publish |
| | `SetSubscription(filter string) error` / `UnsetSubscription(filter string) error` | adds/removes a topic filter; subscriptions accumulate |
| | `SubscriptionAt(index int) (Subscription, bool, error)` | returns the filter and pattern flag as `Subscription{Filter string; Pattern bool}`, or `(Subscription{}, false, nil)` when Core returns `ENOENT` |
| | `TopicsCount() (int, error)` | active subscription count |
| `XPubSocket` | `ReceiveSubscriptionEvent(out *SubscriptionEvent, flags RecvFlags) (bool, error)` | populates `out` with the next subscribe/unsubscribe, added on top of the `PubSocket` surface it embeds via `publishSocket` |
| | `Publish(topic string) PublishOp` | starts the publish builder |

The binding supplies its own buffer on the first call to `zlink_subscription_at`. A successful
call completes the query, including when the filter is empty. If Core returns `ENOBUFS`, the
binding calls Core once more with the required length Core returned. Other Core failures
return `*ConfigError`.

**Completion result.** `Subscribe`/`ReceiveSubscriptionEvent` follow the same `(false,
nil)`-on-`DontWait` convention.

**When to use.** Use `XPubSocket` specifically to observe subscriber churn via
`ReceiveSubscriptionEvent`, or manual admission via `PubSocketOptions.SetManual`/
`ApproveSubscribe`/`RejectSubscribe`. Use `XSubSocket` specifically when subscriptions must be
carried as ordinary messages instead.

---

## `StreamSocket`

Exchanges framed packets directly with raw TCP peers, outside the zlink wire protocol used by
every other socket type. See the shared socket surface above for the methods available on STREAM.

```go
stream, err := ctx.StreamSocket()
stream.SetReceiveMode(zlink.StreamReceivePacket)
var packet zlink.StreamPacket
ok, err := stream.RecvPacket(&packet, zlink.RecvFlagsNone)
```

**Options.** No `StreamSocketOptions` facade type exists — `SetNotify`/`Notify` are declared
directly on `StreamSocket`, unlike `PubSocketOptions`'s separate facade shape.

| Member | Meaning |
| --- | --- |
| `SendTo(target RoutingID) SendOp` | starts the shared send builder, addressed to that peer |
| `Recv(out *Received, flags RecvFlags) (bool, error)` | pulls a raw record when receive mode is `StreamReceiveRaw` |
| `RecvPacket(out *StreamPacket, flags RecvFlags) (bool, error)` | pulls one caller-owned header/body packet in `StreamReceivePacket` mode |
| `SetReceiveMode(StreamReceiveMode) error` / `ReceiveMode() (StreamReceiveMode, error)` | Select RAW or PACKET before bind. The mode is fixed after bind. |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | assigns/reads this socket's own routing id, observed by peers on connect |
| `SetNotify(bool) error` / `Notify() (bool, error)` | delivers peer connect/disconnect as application messages when enabled |

**Completion result.** Both receive forms follow the `(false, nil)`-on-`DontWait` convention.

**When to use.** Select RAW or PACKET before bind, then use the matching pull API. Remember `StreamSocket` cannot
`Connect`/`Disconnect` at all in this binding — it is bind-and-accept only from the public API's
perspective.

---

## Shared flags and enums

| Type | Used by | Values |
|---|---|---|
| `SendFlags` (named `int`) | synchronous publish `.Flags(...)` stage | `SendFlagsNone`, `SendFlagsDontWait` |
| `RecvFlags` (named `int`) | Every `Recv`/`Subscribe`/`ReceiveSubscriptionEvent` | `RecvFlagsNone`, `RecvFlagsDontWait` |
| `RidDuplicatePolicy` (named `int`) | `CommonSocketOptions.RidDuplicatePolicy`/`RouterSocket.SetHandover` | `RidDuplicateReject`, `RidDuplicateHandover` |
| `SubmitRetryMode` (named `int`) | `CommonSocketOptions.SubmitRetryMode` | `SubmitRetryOff`, `SubmitRetryLocalFailure` |

**When to use.** These are plain named `int` types with package-level constants, not a Go `iota`
enum with a `String()` method or a `[Flags]`-style bitmask type — none of them implement
`fmt.Stringer` in this binding.

---

See
[`internal/native/socket_core.go`](../../../../bindings/go/internal/native/socket_core.go),
[`socket_types.go`](../../../../bindings/go/internal/native/socket_types.go), and the
[Go binding spec](../../spec/go/README.en.md) for the full rationale.
