[한국어](02-messaging.ko.md) | English

[Reference index](README.en.md)

# 02. Messaging

This category covers `Message` (frame ownership), the receive envelope types (`Received`,
`TopicMessage`, `SubscriptionEvent`), and the shared send/request/reply operation-builder
interfaces every socket type's entry point returns. `RoutingID` is documented in the Core
category, since it is a package-wide value type, not specific to messaging. The exact signatures
are owned by
[`internal/native/message.go`](../../../../bindings/go/internal/native/message.go),
[`received.go`](../../../../bindings/go/internal/native/received.go),
[`topic_message.go`](../../../../bindings/go/internal/native/topic_message.go),
[`subscription_event.go`](../../../../bindings/go/internal/native/subscription_event.go), and
[`operations.go`](../../../../bindings/go/internal/native/operations.go), re-exported as aliases
by the root package's [`root_projection.go`](../../../../bindings/go/root_projection.go).

---

## `Message`

Owns one native zlink frame — the unit every send, request, reply, and receive API moves.

```go
empty, err := zlink.NewMessage(nil)
sized, err := zlink.NewMessageWithSize(4096)
fromBytes, err := zlink.NewMessage(payload)
fromString, err := zlink.NewMessageString("payload")
```

**Options.**

| Member | Meaning |
| --- | --- |
| `NewMessage(data []byte)` | copies `data`; `nil`/empty produces a zero-length message |
| `NewMessageWithSize(size int)` | writable, zero-initialized storage |
| `NewMessageString(value string)` | UTF-8 encode then copy |
| `Data() []byte` | a view valid only until `Close`; **do not** retain across goroutines or past the message's lifetime — use `Bytes()` for that |
| `Bytes() []byte` | snapshot copy of the payload |
| `Size()` | payload length in bytes |
| `IsEmpty()` | whether `Size()` is zero |
| `Text()` / `String()` | both decode `Data()` as UTF-8 |
| `CopyTo(destination []byte) (int, error)` | copies the payload into a caller-provided slice; errors if `destination` is too small |
| `TryCopyTo(destination []byte) bool` | non-erroring variant of `CopyTo`, discards the error |
| `RefCount() int` | diagnostic only; returns `0` on failure rather than propagating an error |
| `Clone()` / `Copy()` | equivalent — both return `(*Message, error)`, an independent payload copy |
| `Close() error` | releases the message |

**Completion result.** Every member is synchronous. `Close()` returns `nil` for a
`nil` receiver or an already-closed message. The caller closes the message after use.

Passing a message through `Message(...)` consumes it once Core admits the operation. Send and
request also consume it when Core returns a valid WRITABLE wait token. An initial failure before
either outcome preserves the message. `MoveMessage(...)` transfers ownership at submit time.

**When to use.** Use `NewMessageWithSize` or `NewMessage` to build an outbound payload.
Use `Clone()` when an independent message is needed. Use `Data()` to access the payload
while the message is open and `Bytes()` to copy it for use after closing.


---

## `Received`

A received message envelope: its routing metadata, opaque reply token (for a ROUTER request), and
request/reply exchange), and message parts. Reuse one instance across `recv` calls to avoid a
per-receive allocation — the socket's receive method resets and repopulates it.

```go
var received zlink.Received
ok, err := router.Recv(&received, zlink.RecvFlagsNone)
if _, replyable := received.ReplyToken(); ok && replyable {
    received.Reply().Message(reply).Submit(ctx)
}
```

**Options.** No public constructor — callers declare a zero-value `Received{}` and pass its
address to a socket's receive method (Sockets category), which populates it.

| Member | Meaning |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | the peer routing id and whether it's present |
| `ReplyToken() (ReplyToken, bool)` | the opaque reply capability and whether it is present |
| `IsSinglePart() bool` | whether `Parts()` has exactly one element |
| `Parts() []*Message` | every message part this envelope holds |
| `FirstPart() (*Message, error)` | Returns the first part, which remains owned by `Received`. When empty, returns `*RecvError` with `RecvInvalidState` and `EINVAL`. |
| `Reply() ReplyOp` | starts the shared reply builder; only valid when `ReplyToken()` is present |
| `Send() SendOp` | starts the shared send builder, addressed back to this envelope's captured source route |
| `Close() error` | closes every retained part; safe to call on a `nil` receiver |

**Completion result.** All members are synchronous. `Close()` (and the receive method's own reset
step) closes every previously retained part before discarding them — reusing a `Received` without
closing the prior contents never leaks native frames.

**When to use.** Reuse one `Received` across a receive loop by reference rather than allocating a
new one per message. Use `Reply()` rather than reconstructing the destination route by hand — the
routing id and reply token are encapsulated by this builder.

---

## `TopicMessage`

A received publish: its topic, source routing id, and message parts. Reuse one instance across
subscribe-receive calls the same way as `Received`.

```go
var published zlink.TopicMessage
ok, err := sub.Subscribe(&published, zlink.RecvFlagsNone)
topic := published.Topic()
```

**Options.** No public constructor — declare a zero-value `TopicMessage{}`.

| Member | Meaning |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | the publisher's routing id and whether it's present |
| `Topic() string` | the topic this publish was sent on |
| `IsSinglePart() bool` / `Parts() []*Message` / `FirstPart() (*Message, error)` / `Close() error` | same shape as `Received` |

**Completion result.** Synchronous; same close-then-reuse contract as `Received`.

**When to use.** Reuse one instance across a subscribe-receive loop the same way as `Received`.

---

## `SubscriptionEvent`

Reports one subscriber's subscribe or unsubscribe, as observed by an XPUB socket.

```go
var evt zlink.SubscriptionEvent
ok, err := xpub.ReceiveSubscriptionEvent(&evt, zlink.RecvFlagsNone)
```

**Options.** No public constructor — declare a zero-value `SubscriptionEvent{}`. All members are
value receivers, not pointer.

| Member | Meaning |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | the subscriber's routing id and whether it's present |
| `Subscribed() bool` | `true` for a subscribe, `false` for an unsubscribe |
| `Topic() string` | the topic that was subscribed or unsubscribed |

**Completion result.** Synchronous; no `Close()` — this type owns no native resources of its own
(unlike `Received`/`TopicMessage`, which own message parts).

**When to use.** Use on an XPUB socket's subscription-event receive path (Sockets category) to
observe subscriber churn.

---

## Send / request / reply operation-builder shape

The first part added to `SendOp`, `RequestOp`, `ReplyOp`, or `PublishOp` exposes
`Submit(ctx)`. Send, request, and publish accept parts via `Message(*Message)` or `Bytes([]byte)`. Send and publish
also accept `MoveMessage(*Message)`. Request accepts `Timeout(time.Duration)`, and publish accepts
`Flags(SendFlags)`. Reply accepts `Message(*Message)`.

```go
send, err := dealer.Send().Message(part1).Message(part2).Submit(ctx)
if err != nil {
    return err
}
if err := send.Admitted(ctx); err != nil {
    return err
}
```

```go
request, err := dealer.Request().Message(payload).Timeout(5 * time.Second).Submit(ctx)
if err != nil {
    return err
}
parts, err := request.Reply(ctx)
if err != nil {
    return err
}
for _, part := range parts {
    defer part.Close()
}
```

```go
if err := received.Reply().Message(reply).Submit(ctx); err != nil {
    return err
}
```

**Options.** Adding the first part returns the corresponding submit interface.
If its `context.Context` is already canceled or past its deadline, `Submit(ctx)`
returns `ctx.Err()` before native submission.

| Terminal | Return | Meaning |
| --- | --- | --- |
| Send `Submit(ctx)` | `(SendSubmission, error)` | `Admitted(ctx)` waits for admission. |
| Request `Submit(ctx)` | `(RequestSubmission, error)` | `Admitted(ctx)` waits for admission. `Reply(ctx)` waits for the reply and transfers ownership of the returned parts to the caller. |
| Reply `Submit(ctx)` | `error` | Returns the admission result. |
| Publish `Submit(ctx)` | `(bool, error)` | With `SendFlagsDontWait`, `false, nil` reports backpressure. |

**Completion result.** For send and request, `Result()` reports the initial submit outcome.
The caller closes each part returned by `Reply(ctx)`. Once a builder attempts native
submission, a second submit returns an error. Canceling the context can end the Go
wait after submission. The completion owner continues to drain Core completions.

**When to use.** Handle the immediate result of `Submit(ctx)` first. When a public poller
is registered for `PollCompletion`, drive it in a separate goroutine while waiting for
admission or a reply. Use `Received.Send()` for the envelope's captured source route or
`Received.Reply()` to answer a request with a reply token.

---

See [`internal/native/message.go`](../../../../bindings/go/internal/native/message.go),
[`received.go`](../../../../bindings/go/internal/native/received.go),
[`operations.go`](../../../../bindings/go/internal/native/operations.go), and the
[Go binding spec](../../spec/go/README.en.md) for the full rationale.
