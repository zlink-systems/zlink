[한국어](02-messaging.ko.md) | English

[Reference index](README.en.md)

# 02. Messaging

This category covers message ownership, the receive envelope types (`Received`, `TopicMessage`,
`SubscriptionEvent`), and the shared send/request/reply operation-builder family every socket
type's entry point returns. The exact signatures are owned by
[`contracts/messaging/`](../../../../bindings/rust/src/contracts/messaging/).

---

## `Message`

Owns one native zlink frame — the unit every send, request, reply, and receive API moves.

```rust
let empty = Message::new()?;
let sized = Message::with_size(4096)?;
let copy: Message = "payload".try_into()?;
```

**Options.** Constructors and conversions all return `Result<Self, ConfigError>`, copying the
input where applicable.

| Member | Meaning |
| --- | --- |
| `Message::new()` | empty |
| `Message::with_size(size: usize)` / `Message::allocate(size)` | writable, uninitialized storage; `allocate` is an alias for `with_size` |
| `Message::try_from<T: AsRef<[u8]>>(data)` / `TryFrom<&[u8]>` / `TryFrom<Vec<u8>>` / `TryFrom<&str>` | copies the input into message-owned storage |
| `as_bytes()` / `data_mut()` | read-only / writable byte-slice views backed by this instance's storage |
| `size()` | payload length in bytes |
| `is_empty()` | whether `size()` is zero |
| `as_str() -> Result<&str, std::str::Utf8Error>` | payload decoded as UTF-8 |
| `to_vec()` | copy of the payload as `Vec<u8>` |
| `copy_to(&mut [u8]) -> Result<usize, ConfigError>` | copies the payload into a caller-provided slice; errors if the destination is too small — no `try_copy_to` non-erroring variant exists here, unlike other languages |
| `ref_count()` | native reference count, diagnostic only |
| `try_clone() -> Result<Self, ConfigError>` | independent payload copy |

**Completion result.** Every member is synchronous. `Message` implements `Drop`, releasing native
storage automatically; sending a message transfers its native frame to the socket (consuming it in
the builder chain — see the operation-builder shape below), after which reading its payload is no
longer meaningful.

**When to use.** Use `Message::with_size`/`allocate` or a `TryFrom`/`try_from` conversion to build
an outbound payload. Use `try_clone()` when an independent copy is needed rather than moving
ownership. `copy_to` returning `Result` (rather than a `bool`-returning `try_copy_to`) means the
caller must handle the `ConfigError` case explicitly when the destination might be undersized.

---

## `Received`

A received message envelope: its routing metadata and message parts. Owns its parts until dropped
or `close`d; reuse one instance across `recv` calls to avoid a per-receive allocation.

```rust
let mut received = Received::empty();
if dealer.recv(&mut received, RecvFlags::NONE)? {
    if received.request_seq().is_some() {
        received.reply().message(Message::try_from("ok")?).submit()?;
    }
}
```

**Options.** `Received::empty()` is the only public constructor, for caller-provided reusable
storage.

| Member | Meaning |
| --- | --- |
| `is_single_part()` | whether `parts()` has exactly one element |
| `routing_id()` | `Option<&RoutingId>`, present when the receive path provides one |
| `request_seq()` | `Option<u64>`, present when replyable |
| `parts()` | `&[Message]`, every message part this envelope holds |
| `first_part() -> Result<&Message, RecvError>` | the first part, without transferring ownership |
| `single_part()` / `single_part_or_error()` | equivalent — both consume `self` and return `Result<Message, RecvError>`, erroring unless exactly one part |
| `into_parts() -> Vec<Message>` | consumes `self`, transferring ownership of every part |
| `close(self) -> Result<(), CloseError>` | consumes `self`, closes every owned part |
| `reply()` | starts the shared `ReplyOp<Empty>` builder; valid only for envelopes with a request sequence |
| `send()` | starts the shared `SendOp<Empty>` builder, addressed to this envelope's captured source route |

**Completion result.** All members are synchronous. Several methods (`single_part`, `into_parts`,
`close`) take `self` by value, consuming the envelope — Rust's ownership system enforces at compile
time that a consumed `Received` cannot be reused, unlike languages where reuse-after-consume is
only a runtime contract.

**When to use.** Reuse one `Received` across a receive loop (via `&mut`) rather than constructing a
new one per message. Use `first_part()` for a non-consuming peek, `single_part()`/`into_parts()`
when the envelope itself is no longer needed and its parts should be moved out.

---

## `TopicMessage`

A received publish: its topic, source routing id, and message parts. Owns its parts until dropped
or `close`d.

```rust
let mut published = TopicMessage::empty();
if sub.subscribe(&mut published, RecvFlags::NONE)? {
    let topic = published.topic();
}
```

**Options.** `TopicMessage::empty()` is the only public constructor. Instance members mirror
`Received`'s shape.

| Member | Meaning |
| --- | --- |
| `is_single_part()` | whether `parts()` has exactly one element |
| `topic() -> &str` | the topic this publish was sent on |
| `routing_id()` | `Option<&RoutingId>`, the publisher's routing id, present when the receive path provides one |
| `parts()` | `&[Message]`, every message part this publish holds |
| `first_part()` / `single_part()` / `single_part_or_error()` / `into_parts()` / `close(self)` | same shape as `Received` |

**Completion result.** Synchronous; the same consuming-vs-non-consuming member split as `Received`.

**When to use.** Reuse one instance across a subscribe-receive loop the same way as `Received`.

---

## `SubscriptionEvent`

Reports one subscriber's subscribe or unsubscribe, as observed by an XPUB socket.

```rust
let mut evt = SubscriptionEvent::empty();
if xpub.receive_subscription_event(&mut evt, RecvFlags::NONE)? { /* ... */ }
```

**Options.** `SubscriptionEvent::empty()` is the only public constructor.

| Member | Meaning |
| --- | --- |
| `routing_id()` | `Option<&RoutingId>`, the subscriber's routing id, present when the receive path provides one |
| `topic() -> &str` | the topic that was subscribed or unsubscribed |
| `is_subscribed() -> bool` | `true` for a subscribe, `false` for an unsubscribe |

**Completion result.** Synchronous; no `close()` — this type owns no native resources of its own.

**When to use.** Use on an XPUB socket's subscription-event receive path (Sockets category) to
observe subscriber churn.

---

## `SendResult`

The outcome of a non-blocking send, as a small standalone enum.

**Options.**

| Member | Meaning |
| --- | --- |
| `Sent` | the send completed immediately |
| `Backpressured` | the send would have blocked |
| `NotReady` | the destination is not yet ready to accept a send |
| `is_sent() -> bool` | convenience shorthand for `matches!(self, SendResult::Sent)` |

**Completion result.** N/A — a plain value type, not itself returned by any entry point documented
in this reference tier; it exists as a public type in this category's source, distinct from the
`bool` that `SendOp::submit()` (below) actually returns.

**When to use.** Not directly produced by the builder-based send path documented here — see the
Sockets category for whether any lower-level entry point returns this type.

---

## Send / request / reply operation-builder shape

The **typestate-based** fluent builder every socket type's `send`/`publish`/`request`/`reply`
entry point (Sockets category) returns to accumulate parts, flags, and a terminal submit. Unlike
every other language covered so far — which use distinct interface/class types per builder stage
(`SendOperation`/`SendSubmitOperation`, etc.) — Rust expresses the stage transitions as a single
generic type (`SendOp<State>`, `RequestOp<State>`, `ReplyOp<State>`) parameterized by a
zero-sized marker type (`Empty`, `Ready`, `CallbackReady`) that the compiler tracks statically; each
`impl SendOp<Empty> { ... }`/`impl SendOp<Ready> { ... }` block exposes only the methods valid at
that stage.

```rust
dealer.send().message(part1)?.message(part2)?.submit()?;

dealer.request()
    .message(Message::try_from("payload")?)
    .timeout(Duration::from_secs(5))
    .submit(|result| {
        // delivered later; result: Result<Vec<Message>, RequestError>
    })?;

received.reply().message(Message::try_from("ok")?).submit()?;
```

**Options.**

| Stage | Member | Meaning |
| --- | --- | --- |
| `SendOp<Empty>` | `.message(self, Message) -> SendOp<Ready>` | starts the chain, consumes `self`, returns the next-stage type |
| `SendOp<Ready>` | `.message(...)` / `.flags(self, SendFlags) -> Self` / `.submit(self) -> Result<bool, SubmitError>` | add parts, set flags, terminal |
| `RequestOp<Empty>` → `RequestOp<Ready>` | same as `SendOp` + `.timeout(self, Duration) -> Self` | mirrors the send chain, adding a reply-wait timeout |
| `RequestOp<Ready>.flags(self, SendFlags)` | narrows to `RequestOp<CallbackReady>` | exposes the same `message`/`timeout`/`flags`/`submit` methods again — flags may be set more than once at that stage |
| `RequestOp<Ready>::submit` / `RequestOp<CallbackReady>::submit` | `submit<F>(self, callback: F) -> Result<(), SubmitError> where F: FnOnce(Result<Vec<Message>, RequestError>) + Send + 'static` | **both callback-only** — there is no `Future`/async-returning overload in this binding's public contract, unlike every other language covered so far |
| `ReplyOp<Empty>` → `ReplyOp<Ready>` | mirrors `SendOp`, `.submit(self) -> Result<(), SubmitError>` | no flags-narrowing behavior of its own beyond `.flags(self, SendFlags) -> Self` |

**Completion result.** `SendOp::submit()` returns `Result<bool, SubmitError>` — the inner `bool` is
`false` only when `SendFlags::DONT_WAIT` was set and the send would have blocked (back-pressure);
any other failure is the `Err` variant. `ReplyOp::submit()` returns `Result<(), SubmitError>`.
`RequestOp::submit(callback)` returns `Result<(), SubmitError>` for the *dispatch* itself (whether
the request was successfully submitted) — the actual reply-or-failure is delivered later to
`callback` as `Result<Vec<Message>, RequestError>`, on a background dispatch thread. Every builder
consumes its accumulated `Message` parts on a successful submit only.

**When to use.** Because Rust has no async submit path here, use the callback form for any
request/reply — bridge it to an async runtime's channel/oneshot manually if `async`/`.await`
ergonomics are needed at the call site. Use `Received::reply()`/`send()` rather than reconstructing
the destination route by hand.

---

See [`contracts/messaging/`](../../../../bindings/rust/src/contracts/messaging/) and the
[Rust binding spec](../../spec/rust/README.en.md) for the full rationale.
