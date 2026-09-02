[한국어](https://zlink-systems.github.io/zlink/ko/bindings/reference/dotnet/02-messaging/) | English

[Reference index](README.en.md)

# 02. Messaging

This category covers message ownership, the receive envelope types (`Received`, `TopicMessage`,
`SubscriptionEvent`), and the shared send/request/reply operation-builder shape every socket-type
entry point in the Sockets category returns. The exact signatures are owned by
[`Contracts/Messaging/`](../../../../bindings/dotnet/src/Zlink/Contracts/Messaging/).
`Contracts/Messaging/MessageEnvelopeParts.cs` is `internal` and has no public contract entry.

---

## `Message`

Owns one zlink message payload — the unit every send, request, reply, and receive API moves.

```csharp
using Message empty = new Message();
using Message sized = new Message(4096);
using Message copy = Message.From("payload"u8);
using Message fromString = Message.From("hello", Encoding.UTF8);
```

**Options.**

| Member | Parameters | Meaning |
| --- | --- | --- |
| `Message()` | — | empty |
| `Message(int size)` | negative throws `ArgumentOutOfRangeException` | writable storage |
| `Message(ReadOnlySpan<byte>)` / `Message(ReadOnlyMemory<byte>)` | — | snapshot copy |
| `From(byte[])` / `From(ReadOnlySpan<byte>)` / `From(ReadOnlyMemory<byte>)` / `From(ReadOnlySequence<byte>)` | — | static factories, snapshot copy |
| `From(Message)` | — | copies another message's payload |
| `From(string)` / `From(string, Encoding)` | UTF-8 by default | encode |
| `Size` / `IsEmpty` / `RefCount` | — | diagnostics |
| `AsSpan()` / `AsReadOnlySpan()` | — | writable/read-only view backed by this instance's storage |
| `AsReadOnlyMemory()` | — | native-backed messages copy into managed memory here |
| `ToArray()` / `GetString()` / `GetString(Encoding)` | — | independent managed copy |
| `CopyTo(Span<byte>)` / `CopyTo(IBufferWriter<byte>)` / `TryCopyTo(Span<byte>, out int)` | — | copy into caller buffer |

**Completion result.** All synchronous. `Message` is `IDisposable`/`IAsyncDisposable` — disposal
releases payload storage. A submit that consumes this message (Sockets/Messaging builder shape
below) leaves the managed instance empty afterward; reading its payload then throws, but disposing
it stays safe and is still required.

**When to use.** A sized or snapshot-copy constructor/factory to build an outbound payload;
`AsSpan()`/`AsReadOnlySpan()` to read or write in place without an extra copy; `ToArray()`/
`GetString()` when an independent managed copy is acceptable.

---

## `Received.Create()`

Creates a reusable receive envelope for the caller-provided-storage receive shape.

```csharp
using Received received = Received.Create();
bool ok = dealer.Recv(received);
```

**Options.** No parameters — `Received` has no public constructor, only `Create()`.

**Completion result.** Returns `Received` synchronously; the caller owns and must dispose it. A
receive API (Sockets category) overwrites its internal state on each successful call.

**When to use.** Create one `Received` per receive loop/thread and reuse it across calls instead
of allocating a fresh instance per message.

---

## `Received` members

Reads envelope metadata and message parts, or starts a reply/send addressed to the envelope's
source.

```csharp
if (received.ReplyToken is { } token)
{
    received.Reply().Message(Message.From("ok")).Submit();
}
Message first = received.FirstPart();
```

**Options.**

| Member | Returns | Meaning |
| --- | --- | --- |
| `RoutingId` | `RoutingId?` | present when the receive path provides one |
| `ReplyToken` | `ReplyToken?` | opaque capability present on a ROUTER request |
| `MessageType` | `ReceivedMessageType` | `Raw`/`Request`/`Reply`/`ErrorReply` |
| `Parts` | `IReadOnlyList<Message>` | every message part this envelope holds |
| `IsSinglePart` | `bool` | whether `Parts` has exactly one element |
| `FirstPart()` | `Message` | the first part, without transferring ownership |
| `SinglePartOrThrow()` | `Message` | the single part, throws if `Parts` has more than one |
| `Reply()` | builder | valid only when `ReplyToken` has a value — see the shared builder shape below |
| `Send()` | builder | addressed to the envelope's source route |

**Completion result.** All synchronous. `Dispose()` releases message parts owned by this envelope
unless another API already transferred their ownership. `Reply()`/`Send()` return a builder from
the shared operation-builder shape below.

**When to use.** Branch on `MessageType`/`ReplyToken` to decide whether an envelope is replyable.
Use `Reply()` to answer a request in place instead of looking up the source route separately.

---

## `TopicMessage`

Holds one received publish: its topic, source routing id, and message parts.

```csharp
using TopicMessage published = new TopicMessage();
bool ok = sub.Recv(published);
string topic = published.Topic;
```

**Options.**

| Member | Returns | Meaning |
| --- | --- | --- |
| `TopicMessage()` | — | public constructor (unlike `Received`, not a factory) |
| `ReleaseForReuse()` | `void` | releases the current parts and metadata while retaining internal topic receive buffers for a later `Subscribe` call; throws `ObjectDisposedException` after terminal `Dispose()` |
| `RoutingId` | `RoutingId?` | the publisher's routing id, present when the receive path provides one |
| `Topic` | `string` | decoded lazily from topic bytes |
| `Parts` | `IReadOnlyList<Message>` | every message part this publish holds |
| `IsSinglePart` | `bool` | whether `Parts` has exactly one element |
| `FirstPart()` / `SinglePartOrThrow()` | `Message` | same shape as `Received` — first part without transfer, or the single part (throws if multipart) |

**Completion result.** Synchronous. `Dispose()` releases the parts and topic buffers this
instance owns. `ReleaseForReuse()` releases the current parts and metadata but keeps the
internal topic receive buffers so a caller can reuse the same storage in a later `Subscribe`
call. The caller must not inspect the old parts after releasing them. `ReleaseForReuse()` may
be called repeatedly while the instance is open; after terminal `Dispose()`, it throws
`ObjectDisposedException` and does not reopen the instance.

**When to use.** Reuse one instance across a subscribe-receive loop the same way `Received` is
reused.

---

## `SubscriptionEvent`

Reports one subscriber's subscribe or unsubscribe, as observed by an XPUB socket.

```csharp
using SubscriptionEvent evt = new SubscriptionEvent();
bool ok = xpub.Recv(evt);
```

**Options.**

| Member | Returns | Meaning |
| --- | --- | --- |
| `SubscriptionEvent()` | — | public constructor |
| `RoutingId` | `RoutingId?` | the subscriber's routing id, present when the receive path provides one |
| `Topic` | `string` | the topic that was subscribed or unsubscribed |
| `Subscribed` | `bool` | `true` for a subscribe, `false` for an unsubscribe |
| `SubscriptionEntry(string Filter, bool IsPattern)` | record | one active subscription |

**Completion result.** Synchronous; no disposal — this type owns no native resources.

**When to use.** On an XPUB socket's subscription-event receive path (Sockets category) to observe
subscriber churn.

---

## Send / request / reply operation-builder shape

The fluent builder every `Send`, routed send, `Publish`, `Request`, and `Reply` entry point (all in
the Sockets category) returns to accumulate parts, flags, and a terminal submit.

```csharp
dealer.Send(routingId).Message(Message.From("part-1")).Message(Message.From("part-2")).Submit();

IReadOnlyList<Message> reply = await dealer
    .Request(routingId)
    .Message(Message.From("payload"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async();

received.Reply().Message(Message.From("ok")).Submit();
```

**Options.**

| Stage | Member | Meaning |
| --- | --- | --- |
| `SendOperation` | `.Message(Message)` | starts the chain |
| `SendSubmitOperation` | `.Message(...)` / `.Submit()` / `.Async(CancellationToken)` | add parts, then choose blocking or Task terminal |
| `RequestOperation`/`RequestSubmitOperation` | same as `Send` + `.Timeout(TimeSpan)` | mirrors the send chain, adding a reply-wait timeout |
| `RequestSubmitOperation` terminals | `.Submit()` / `.Async(CancellationToken)` | blocking reply result or completion-backed Task reply result |
| `ReplyOperation`/`ReplySubmitOperation` | mirrors `Send`, no flags stage | core reply function takes no send-flag argument |
| `Messages(IReadOnlyList<Message>)` | `MessageOperations` extension | adds several parts in order at any stage of all four families; not an independent entry point |

**Completion result.** All calls submit binding-owned native staging parts. A successful submit
consumes the public parts; on failure the public parts remain with the caller even though Core
consumes each native staging part actually passed to a synchronous submit.

| Terminal | Returns | Meaning |
| --- | --- | --- |
| `SendSubmitOperation.Submit()` | `void` | blocks for Core local admission; failures throw `ZlinkException` |
| `SendSubmitOperation.Async(CancellationToken)` | `Task` | DONTWAIT submit settled from the socket completion queue |
| `ReplySubmitOperation.Submit()` | `void` | throws `ZlinkException` on failure |
| `RequestSubmitOperation.Async(CancellationToken)` | `Task<IReadOnlyList<Message>>` | the caller owns and must dispose the reply messages |
| `RequestSubmitOperation.Submit()` | `IReadOnlyList<Message>` | blocks until the completion queue yields the reply; caller disposes parts |

**When to use.** `.Async()` in async code and `.Submit()` on a thread that may block. Use `Reply()`/`Send()` from
a `Received` envelope rather than reconstructing the destination route by hand.

---

See [`Contracts/Messaging/`](../../../../bindings/dotnet/src/Zlink/Contracts/Messaging/) and the
[.NET binding spec](../../spec/dotnet/README.en.md) for the full rationale.
