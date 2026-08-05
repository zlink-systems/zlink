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
through [`contracts/messaging.go`](../../../../bindings/go/contracts/messaging.go).

---

## `Message`

Owns one native zlink frame — the unit every send, request, reply, and receive API moves.

```go
empty, err := contracts.NewMessage(nil)
sized, err := contracts.NewMessageWithSize(4096)
fromBytes, err := contracts.NewMessage(payload)
fromString, err := contracts.NewMessageString("payload")
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

**Completion result.** Every member is synchronous. `Close()` is idempotent (a `nil` receiver or
already-closed message returns `nil`); the caller must call it explicitly — there is no finalizer.
Sending a message via a `MoveMessage`-family builder call transfers its native frame to the socket
and marks it closed on the Go side (see the operation-builder shape below); sending via a plain
`Message`-family call leaves the caller's `Message` still owned and still requiring `Close()`.

**When to use.** Use `NewMessageWithSize`/`NewMessage` to build an outbound payload; use `Clone()`
when an independent copy is needed rather than transferring ownership through `MoveMessage`. Prefer
`Data()` over `Bytes()` on the hot path when the slice is consumed before the next `recv` call —
`Bytes()` always allocates a copy.

---

## `Received`

A received message envelope: its routing metadata, request sequence (if this receive came from a
request/reply exchange), and message parts. Reuse one instance across `recv` calls to avoid a
per-receive allocation — the socket's receive method resets and repopulates it.

```go
var received contracts.Received
ok, err := dealer.Recv(&received, contracts.RecvFlagsNone)
if ok && received.HasRequestSeq() {
    received.Reply().Message(reply).Submit(ctx)
}
```

**Options.** No public constructor — callers declare a zero-value `Received{}` and pass its
address to a socket's receive method (Sockets category), which populates it.

| Member | Meaning |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | the peer routing id and whether it's present |
| `RequestSeq() uint64` / `HasRequestSeq() bool` | the request sequence and whether it's present |
| `IsSinglePart() bool` | whether `Parts()` has exactly one element |
| `Parts() []*Message` | every message part this envelope holds |
| `FirstPart() (*Message, error)` | the first part, without ownership transfer — the part remains owned by `Received` |
| `Reply() ReplyOp` | starts the shared reply builder; only valid when `HasRequestSeq()` is true — `Submit` on the resulting builder returns `*SubmitError` otherwise |
| `Send() SendOp` | starts the shared send builder, addressed back to this envelope's captured source route |
| `Close() error` | closes every retained part; safe to call on a `nil` receiver |

**Completion result.** All members are synchronous. `Close()` (and the receive method's own reset
step) closes every previously retained part before discarding them — reusing a `Received` without
closing the prior contents never leaks native frames.

**When to use.** Reuse one `Received` across a receive loop by reference rather than allocating a
new one per message. Use `Reply()` rather than reconstructing the destination route by hand — the
routing id and request sequence are encapsulated and inaccessible outside this builder.

---

## `TopicMessage`

A received publish: its topic, source routing id, and message parts. Reuse one instance across
subscribe-receive calls the same way as `Received`.

```go
var published contracts.TopicMessage
ok, err := sub.Subscribe(&published, contracts.RecvFlagsNone)
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
var evt contracts.SubscriptionEvent
ok, err := xpub.ReceiveSubscriptionEvent(&evt, contracts.RecvFlagsNone)
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

The fluent builder interfaces every socket type's `Send`/`Publish`/`Request`/`Reply` entry point
(Sockets category) returns to accumulate parts, flags, and a terminal submit. Every stage is a
distinct Go interface (`SendOp` → `SendSubmitOp`, `RequestOp` → `RequestSubmitOp` →
`RequestCallbackSubmitOp`, `ReplyOp` → `ReplySubmitOp`) — calling a stage-appropriate method
returns the next interface in the chain, so a caller cannot call `Submit` before at least one part
has been added.

```go
ok, err := dealer.Send().Message(part1).Message(part2).Submit(ctx)

ch, err := dealer.Request().
    Message(payload).
    Timeout(5 * time.Second).
    SubmitAsync(ctx)
completion := <-ch

ok, err := dealer.Request().
    Message(payload).
    Submit(ctx, func(result contracts.RequestResult, parts []*contracts.Message) {
        // delivered later, on a background dispatch goroutine
    })

err := received.Reply().Message(reply).Submit(ctx)
```

**Options.** **Every terminal `Submit`/`SubmitAsync` takes a `context.Context` as its first
argument** — a cancelled or deadline-exceeded context short-circuits the call with that context's
`Err()` before the native submit runs, which no other language's operation builder does.

| Stage | Member | Meaning |
| --- | --- | --- |
| `SendOp` | `.Message(*Message)` / `.MoveMessage(*Message)` / `.Bytes([]byte)` → `SendSubmitOp` | start the chain; `MoveMessage` transfers the message's ownership to the socket on a successful submit |
| `SendSubmitOp` | same three add methods + `.Flags(SendFlags)` / `.Submit(ctx context.Context) (bool, error)` | add parts, set flags, terminal |
| `RequestOp` | `.Message` / `.Bytes` → `RequestSubmitOp` | start the request chain |
| `RequestSubmitOp` | `.Timeout(time.Duration)` | adds a reply-wait timeout |
| `RequestSubmitOp.Flags(SendFlags)` | narrows to `RequestCallbackSubmitOp` | callback-only stage |
| `RequestSubmitOp` terminals | `.SubmitAsync(ctx) (<-chan RequestReplyCompletion, error)` and `.Submit(ctx, callback RequestReplyCallback) (bool, error)` | **both exposed at once** — no other language covered so far exposes both an async-channel and a callback terminal on the same builder stage |
| `RequestCallbackSubmitOp` | repeats `Message`/`Bytes`/`Timeout`/`Flags` + only the callback `Submit` | the `SubmitAsync` channel path is gone once narrowed |
| `ReplyOp` | `.Message(*Message)` → `ReplySubmitOp` | starts the reply chain |
| `ReplySubmitOp` | `.Flags(SendFlags)` / `.Submit(ctx context.Context) error` | set flags, terminal |

**Completion result.** `SendSubmitOp.Submit` returns `(bool, error)` — the `bool` is `false` only
when `SendFlagsDontWait` was set and the send would have blocked (back-pressure); any other
failure is the `error`. `ReplySubmitOp.Submit` returns `error` alone. `RequestSubmitOp.Submit`/
`RequestCallbackSubmitOp.Submit` return `(bool, error)` for the *dispatch* itself (whether the
request was accepted) — the actual reply or failure arrives later as a `RequestReplyCompletion`
(`{Result RequestResult; Parts []*Message; Err error}`) sent on the `SubmitAsync` channel, or as
the `(RequestResult, []*Message)` arguments to the `Submit` callback, delivered on a background
dispatch goroutine either way. Every builder is single-use — calling `Submit`/`SubmitAsync` a
second time on the same builder value returns an error rather than resubmitting.

**When to use.** Use `SubmitAsync` when the call site already communicates via channels/`select`;
use the callback `Submit` when integrating with code that expects a callback instead. Use
`Received.Reply()`/`Send()` rather than reconstructing the destination route by hand — the routing
id (and request sequence, for `Reply`) are encapsulated and inaccessible outside the builder.

---

See [`internal/native/message.go`](../../../../bindings/go/internal/native/message.go),
[`received.go`](../../../../bindings/go/internal/native/received.go),
[`operations.go`](../../../../bindings/go/internal/native/operations.go), and the
[Go binding spec](../../spec/go/README.en.md) for the full rationale.
