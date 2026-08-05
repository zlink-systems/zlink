[한국어](03-sockets.ko.md) | English

[Reference index](README.en.md)

# 03. Sockets

This category covers `CommonSocketOptions`/`PubSocketOptions`, the eight concrete socket types, and
the shared lifecycle surface they get through Go struct embedding. **There is no shared
cross-socket-type interface** — `PairSocket`/`PubSocket`/`SubSocket`/`DealerSocket`/`RouterSocket`/
`XPubSocket`/`XSubSocket` each embed an unexported base (`directSocket`/`publishSocket`/
`subscribeSocket`/`routedSocket`), which in turn embeds `connectionSocket`, which embeds
`socketCore` — `Bind`/`Connect`/`Unbind`/`Disconnect`/`DisconnectRID`/`Close` are promoted from
`socketCore` automatically through this chain, not redeclared per type. `StreamSocket` breaks the
chain: it holds its base as a **named field** (`core *routedSocket`), not an embedded one, so
nothing is promoted — every method it exposes is a hand-written one-line forward, and several
methods every other socket type gets for free (`Connect`, `Disconnect`, `DisconnectRID`,
`CommonOptions()`, and therefore every `CommonSocketOptions` accessor not individually forwarded)
are simply absent from `StreamSocket`. The exact signatures are owned by
[`internal/native/socket_core.go`](../../../../bindings/go/internal/native/socket_core.go),
[`socket_types.go`](../../../../bindings/go/internal/native/socket_types.go),
[`socket_options.go`](../../../../bindings/go/internal/native/socket_options.go),
[`connection_socket.go`](../../../../bindings/go/internal/native/connection_socket.go), and the
per-type files (`socket_direct.go`, `socket_routed.go`, `socket_publish.go`,
`socket_subscribe.go`, `socket_completion_control.go`), re-exported as aliases through
[`contracts/sockets.go`](../../../../bindings/go/contracts/sockets.go). Socket creation itself is
documented in the Core category (`Context.PairSocket()`, etc.), and the `Send`/`Request`/`Reply`
builder family every socket below returns is documented in the Messaging category.

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

`StreamSocket` hand-forwards only `Bind`, `Unbind`, `Close`, `LastEndpoint`, `SetTLSServer`,
`SetTLSClient`, and a handful of individual `CommonSocketOptions` values
(`SetSendHighWaterMark`/`SendHighWaterMark`, `SetReceiveHighWaterMark`/`ReceiveHighWaterMark`,
`SetLinger`, `SetReceiveTimeout`, `SetSendTimeout`, `SetTCPKeepalive`, `SetTCPNoDelay`, `SetIPv6`)
— it has no `Connect`, `Disconnect`, `DisconnectRID`, or `CommonOptions()` at all, and therefore no
way to reach `Immediate`, `RidDuplicatePolicy`, `MaxMessageSize`, `Backlog`, `ReconnectInterval`,
`ConnectTimeout`, or the `SubmitRetry*` family that every other socket type can set.

**Completion result.** Every one of these methods returns `error` (or `(T, error)` for getters).

**When to use.** Treat `StreamSocket`'s narrower surface as a hard constraint, not an oversight to
route around — options it doesn't forward are not reachable through any other public path on this
type.

---

## `CommonSocketOptions` / `PubSocketOptions`

The typed options facades — `CommonSocketOptions` shared by every socket type that embeds
`connectionSocket` (reached via `.CommonOptions()`), `PubSocketOptions` specific to PUB/XPUB
(reached via `.PubOptions()`).

```go
opts := dealer.CommonOptions()
opts.SetSendHighWaterMark(100_000)
opts.SetLinger(time.Second)
opts.SetSubmitRetryMode(contracts.SubmitRetryLocalFailure)
```

**Options — `CommonSocketOptions`.** Every accessor returns `(T, error)`/`error`.

| Member | Meaning |
| --- | --- |
| `Linger()` / `SetLinger(time.Duration)` | upper bound on how long `Close()` waits for pending sends to flush |
| `SendHighWaterMark()` / `SetSendHighWaterMark(int)` | outbound accounted-byte HWM |
| `ReceiveHighWaterMark()` / `SetReceiveHighWaterMark(int)` | inbound accounted-byte HWM |
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
pair.Send().Message(ping).Submit(ctx)
var received contracts.Received
ok, err := pair.Recv(&received, contracts.RecvFlagsNone)
```

**Options.** Plus the shared lifecycle/TLS/options surface above.

| Member | Meaning |
| --- | --- |
| `Send() SendOp` | starts the shared send builder |
| `Recv(out *Received, flags RecvFlags) (bool, error)` | populates `out` with the next message |
| `OnSendReady(handler func()) error` | registers a back-pressure-cleared callback |

**Completion result.** `Recv` returns `(false, nil)` only when `RecvFlagsDontWait` is set and no
message is available.

**When to use.** Use PAIR for an exclusive point-to-point link — it has no peer routing and does
not load-balance.

---

## `DealerSocket`

Load-balances sends across its connected peers and can issue routed requests.

```go
dealer, err := ctx.DealerSocket()
dealer.SetRoutingID(contracts.NewRoutingIDString("worker-3"))
dealer.Request().Message(payload).Submit(ctx, func(result contracts.RequestResult, parts []*contracts.Message) {
    // ...
})
```

**Options.** Same shared surface as `PairSocket`, plus:

| Member | Meaning |
| --- | --- |
| `Send() SendOp` | starts the shared send builder |
| `Recv(out, flags) (bool, error)` | populates `out` with the next message |
| `Request() RequestOp` | starts the shared request builder; no target parameter — DEALER has no API-level peer routing id |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | assigns/reads this socket's own routing id, observed by peers on connect |
| `SetProbe(bool) error` | **set-only, no getter**; sends an empty probe on connect |
| `Weight()` / `SetWeight(int)` | load-balancing weight, both directions |
| `SetRequestTimeout(time.Duration) error` | **set-only, no getter** — matching the same asymmetry in every other language's Dealer type |

**Completion result.** `Recv` follows the same `(false, nil)`-on-`DontWait` convention as
`PairSocket`.

**When to use.** Set `SetRoutingID` before connecting so peers observe it from the first message.
DEALER has no protocol envelope helper to reply to an arbitrary token — reply from a received
request's context (`Received.Reply()`) or use ROUTER's explicit reply surface instead.

---

## `RouterSocket`

Routes messages to peers addressed by routing id, replies to a specific peer's request, and
additionally supports an opaque completion-control channel.

```go
router, err := ctx.RouterSocket()
router.SendTo(peerRID).Message(hello).Submit(ctx)
router.OnCompletionControl(func(received *contracts.Received) {
    defer received.Close()
})
```

**Options.** Same shared surface as `PairSocket`, plus. **This binding does implement
Completion-control on ROUTER**, unlike rust, which declares no equivalent public entry point.

| Member | Meaning |
| --- | --- |
| `SendTo(target RoutingID) SendOp` | starts the shared send builder, addressed to that peer |
| `Recv(out, flags) (bool, error)` | populates `out` with the next message; returns `*RecvError{Result: RecvBusy}` if a receive-callback handler is already installed — this binding's `Recv`/callback path are mutually exclusive on ROUTER |
| `Request(peerRID RoutingID) RequestOp` | Messaging category's `RequestOp`, addressed to a specific peer |
| `Reply(rid RoutingID, requestSeq uint64) ReplyOp` | Messaging category's `ReplyOp`, answering that peer's request |
| `OnSendReady(handler func()) error` | registers a back-pressure-cleared callback |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | assigns/reads this socket's own routing id, observed by peers on connect |
| `SetMandatory(bool) error` | **set-only**; error instead of silent drop on an unknown route |
| `SetProbe(bool) error` | **set-only**; sends an empty probe on connect |
| `SetHandover(bool) error` | **set-only** convenience over `CommonOptions().SetRidDuplicatePolicy`, mapping `true` to `RidDuplicateHandover` — both paths reach the same underlying option |
| `SetConnectRoutingID(RoutingID) error` | **set-only — no getter for the assigned connect routing id**, unlike dotnet's/cpp's read-only `ConnectRoutingId` property |
| `Weight()` / `SetWeight(int)` | load-balancing weight, both directions |
| `RequestTimeout()` / `SetRequestTimeout(time.Duration)` | request timeout, **both directions, unlike Dealer's set-only** |
| `OnCompletionControl(handler func(*Received)) error` | registers the callback that receives incoming completion-control records |
| `CompletionControl(peerRID RoutingID) SendOp` | an opaque control-plane record addressed to a peer; the control plane accepts no send flags — `Flags` values other than `SendFlagsNone` are rejected at submit time |

**Completion result.** `Recv` follows the same convention as `PairSocket`.

**When to use.** Use `Request(peerRID)`/`Reply(rid, requestSeq)` for ROUTER-initiated or
ROUTER-answered request/reply, where DEALER cannot address a specific peer. Use
`OnCompletionControl`/`CompletionControl` for out-of-band control messages that should not be
confused with ordinary application payloads.

---

## `PubSocket` / `SubSocket` / `XPubSocket` / `XSubSocket`

PUB publishes topic-filtered messages, dropping ones with no matching subscriber; SUB subscribes
with subscriptions set as socket options; XPUB adds subscriber-event surfacing, XSUB carries
subscriptions as messages instead. `PubSocket`/`XPubSocket` both embed `publishSocket`; `SubSocket`/
`XSubSocket` both embed `subscribeSocket`.

```go
pubSocket, err := ctx.PubSocket()
pubSocket.Publish("prices").Message(tick).Submit(ctx)

sub, err := ctx.SubSocket()
sub.SetSubscription("prices.")
var msg contracts.TopicMessage
ok, err := sub.Subscribe(&msg, contracts.RecvFlagsNone)
```

**Options.** **Neither `PubSocket` nor `XPubSocket` declares `SetRoutingID`/`RoutingID`** — no
routing-id surface at all on either type in this binding, the same as every other language covered
so far. **Neither `SubSocket` nor `XSubSocket` declares `OnSendReady`** — the only socket types in
this binding without it.

| Type | Member | Meaning |
| --- | --- | --- |
| `PubSocket` | `Publish(topic string) SendOp` | starts the shared send builder |
| | `OnSendReady(handler func()) error` | registers a back-pressure-cleared callback |
| | `PubOptions() *PubSocketOptions` | the per-type options facade |
| `SubSocket` / `XSubSocket` | `Subscribe(out *TopicMessage, flags RecvFlags) (bool, error)` | populates `out` with the next matching publish |
| | `SetSubscription(filter string) error` / `UnsetSubscription(filter string) error` | adds/removes a topic filter; subscriptions accumulate |
| | `SubscriptionAt(index int) (string, bool, error)` | a `(filter, isPattern, error)` triple — the filter at that index |
| | `TopicsCount() (int, error)` | active subscription count |
| `XPubSocket` | `ReceiveSubscriptionEvent(out *SubscriptionEvent, flags RecvFlags) (bool, error)` | populates `out` with the next subscribe/unsubscribe, added on top of the `PubSocket` surface it embeds via `publishSocket` |
| | `Publish(topic string) SendOp` | its own copy, not reused from `PubSocket`, though identical in shape |

**Completion result.** `Subscribe`/`ReceiveSubscriptionEvent` follow the same `(false,
nil)`-on-`DontWait` convention.

**When to use.** Use `XPubSocket` specifically to observe subscriber churn via
`ReceiveSubscriptionEvent`, or manual admission via `PubSocketOptions.SetManual`/
`ApproveSubscribe`/`RejectSubscribe`. Use `XSubSocket` specifically when subscriptions must be
carried as ordinary messages instead.

---

## `StreamSocket`

Exchanges framed packets directly with raw TCP peers, outside the zlink wire protocol used by
every other socket type. Wraps its base by a named field, not embedding — see the note at the top
of this category for what that omits.

```go
stream, err := ctx.StreamSocket()
stream.OnPacket(func(routingID contracts.RoutingID, header, body *contracts.Message) {
    // owns header/body
})
```

**Options.** No `StreamSocketOptions` facade type exists — `SetNotify`/`Notify` are declared
directly on `StreamSocket`, unlike `PubSocketOptions`'s separate facade shape.

| Member | Meaning |
| --- | --- |
| `SendTo(target RoutingID) SendOp` | starts the shared send builder, addressed to that peer |
| `Recv(out *Received, flags RecvFlags) (bool, error)` | populates `out` with the next packet; this binding's `Recv` additionally captures the source routing id as a send context on `out`, so a subsequent `out.Send()` addresses the packet's sender — a STREAM-specific enrichment |
| `OnPacket(handler func(RoutingID, *Message, *Message)) error` | registers a callback-driven packet loop; the handler owns both `header` and `body` |
| `OnSendReady(handler func()) error` | registers a back-pressure-cleared callback |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | assigns/reads this socket's own routing id, observed by peers on connect |
| `SetNotify(bool)` / `Notify() (bool, error)` | delivers peer connect/disconnect as application messages when enabled |

**Completion result.** `Recv` follows the `(false, nil)`-on-`DontWait` convention.

**When to use.** Use `OnPacket` for a callback-driven packet loop. Remember `StreamSocket` cannot
`Connect`/`Disconnect` at all in this binding — it is bind-and-accept only from the public API's
perspective.

---

## Shared flags and enums

| Type | Used by | Values |
|---|---|---|
| `SendFlags` (named `int`) | Every send/request/reply builder's `.Flags(...)` stage (Messaging category) | `SendFlagsNone`, `SendFlagsDontWait` |
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
