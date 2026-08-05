[한국어](02-messaging.ko.md) | English

[Reference index](README.en.md)

# 02. Messaging

This category covers message ownership, the receive envelope types (`Received`, `TopicMessage`,
`SubscriptionEvent`), and the shared send/request/reply operation-builder family every socket
type's entry point returns. The exact signatures are owned by
[`contracts/messaging/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/messaging/).

---

## `Message`

Owns one native zlink frame — the unit every send, request, reply, and receive API moves. Java
does not expose borrowed-payload wrappers, because native queue lifetime is not safely bounded by
Java object reachability — every `from(...)` factory copies into message-owned storage.

```java
Message empty = new Message();
Message sized = new Message(4096);
Message copy = Message.from("payload".getBytes(StandardCharsets.UTF_8));
Message fromString = Message.from("hello");
```

**Options.**

| Member | Meaning |
| --- | --- |
| `Message()` | empty |
| `Message(int size)` | writable storage |
| `allocate(int)` | static factory, same as `Message(int)` |
| `from(byte[])` / `from(byte[] data, int offset, int length)` | copies the full array, or the selected range |
| `from(Message)` | copies another message's payload |
| `from(String)` | UTF-8 encode |
| `from(ByteBuffer)` | copies remaining bytes without mutating the source cursor |
| `from(io.netty.buffer.ByteBuf)` | Netty interop, copies readable bytes without mutating the source cursor |
| `size()` | payload length in bytes |
| `more()` | multipart-continuation flag |
| `refCount()` | native reference count, diagnostic only |
| `empty()`/`isEmpty()` | whether `size()` is zero |
| `data()`/`toByteArray()` | copy of the payload as `byte[]` |
| `toUtf8String()` | payload decoded as UTF-8 |
| `dataBuffer()` | read-only `ByteBuffer` view backed by this instance's storage |
| `mutableDataBuffer()` | writable `ByteBuffer` view backed by this instance's storage |
| `copyTo(byte[])` / `copyTo(byte[], int offset)` / `copyTo(byte[] dst, int srcOffset, int dstOffset, int length)` | copies the payload (or a range) into a caller-provided array |
| `copyTo(ByteBuffer)` / `copyTo(ByteBuf)` | copies the payload into a caller-provided buffer |
| `tryCopyTo(ByteBuffer)` / `tryCopyTo(ByteBuf)` | bounds-checked variant, returns `boolean` instead of throwing |
| `copyFrom(byte[]|Message, int srcOffset, int dstOffset, int length)` | copies bytes into this message's storage from a source |
| `readByte`/`readIntLe`/`readIntBe`/`readLongLe`/`writeByte`/`writeIntLe`/`writeIntBe`/`writeLongLe` | in-place binary accessors for parsing/writing a wire format directly against message storage |
| `fill(byte)` / `fill(byte, offset, length)` | overwrites the payload (or a range) with a repeated byte |
| `contentEquals(byte[])` | payload equality check |
| `closeAll(Message[])` / `closeAll(Iterable<? extends Message>)` | static; closes every part in one call, silently ignoring individual close failures |

**Completion result.** Every member is synchronous. `Message implements AutoCloseable`; sending a
message transfers its native frame to the socket, invalidating the instance for further reads —
`close()` releases a message that will not be sent. Out-of-range offsets/lengths throw
`IndexOutOfBoundsException`.

**When to use.** A sized constructor or a copying `from(...)` factory to build an outbound payload.
The in-place binary accessors to parse/write a wire format directly against message storage
without an intermediate `byte[]`. `closeAll(...)` to release every part of a received or
constructed multipart array in one call instead of a hand-written loop.

---

## `Received`

Aggregates one recv result: an optional routing id, request sequence, and the owned message parts.
The returned `parts()` view is immutable and does not copy the underlying array.

```java
Received received = new Received();
if (dealer.recv(received)) {
    received.requestSeq().ifPresent(seq ->
        received.reply().message(Message.from("ok")).submit());
}
```

**Options.** Public no-arg constructor `Received()` for caller-provided storage — the binding
overwrites internal state in place on each successful receive, avoiding a per-recv allocation.

| Member | Returns | Meaning |
| --- | --- | --- |
| `getRoutingId()` | `Optional<RoutingId>` | present when the receive path provides one |
| `requestSeq()` | `Optional<Long>` | present when replyable |
| `parts()` | `List<Message>`, immutable view | every message part this envelope holds |
| `isSinglePart()` | `boolean` | whether `parts()` has exactly one element |
| `firstPart()` | `Message` | the first part, without transferring ownership |
| `singlePartOrThrow()` | `Message` | the single part, throws if `parts()` doesn't have exactly one |
| `reply()` | builder | starts the shared `ReplyOperation`; throws `ZlinkSubmitException` on `submit()` if there is no valid reply context |
| `send()` | builder | starts the shared `SendOperation`, addressed to this envelope's captured source route |

`Received implements AutoCloseable`; `close()` closes every owned part.

**Completion result.** All synchronous. `firstPart()`/`singlePartOrThrow()` throw
`ZlinkRecvException` when there is no data or the part count doesn't match, respectively —
mirroring the receive-side result codes in the Errors category.

**When to use.** Reuse one `Received` across a receive loop. Check `requestSeq()` before calling
`reply()` to confirm the envelope is actually replyable.

---

## `TopicMessage`

Topic-aware recv result used by raw subscription paths: a received publish's topic, source routing
id, and message parts.

```java
TopicMessage published = new TopicMessage();
if (sub.subscribe(published)) {
    String topic = published.topic();
}
```

**Options.** Public no-arg constructor `TopicMessage()`.

| Member | Returns | Meaning |
| --- | --- | --- |
| `getRoutingId()` | `Optional<RoutingId>` | the publisher's routing id, present when the receive path provides one |
| `topic()` | `String` | the topic this publish was sent on |
| `parts()` | `List<Message>` | every message part this publish holds |
| `isSinglePart()` / `firstPart()` / `singlePartOrThrow()` | — | same shape as `Received` |

`TopicMessage implements AutoCloseable`.

**Completion result.** Synchronous; `firstPart()`/`singlePartOrThrow()` throw `ZlinkRecvException`
the same way as `Received`'s equivalents.

**When to use.** Reuse one instance across a subscribe-receive loop the same way as `Received`.

---

## `SubscriptionEvent` / `SubscriptionEntry`

Reports one subscriber's subscribe or unsubscribe (as observed by an XPUB socket), and describes
one active subscription entry.

```java
SubscriptionEvent evt = new SubscriptionEvent();
if (xpub.receiveSubscriptionEvent(evt)) { /* ... */ }
```

**Options.** `SubscriptionEvent()` public no-arg constructor.

| Type | Member | Meaning |
| --- | --- | --- |
| `SubscriptionEvent` | `getRoutingId()` (`Optional<RoutingId>`) | the subscriber's routing id, present when the receive path provides one |
| | `topic()` (`String`) | the topic that was subscribed or unsubscribed |
| | `subscribed()` (`boolean`) | `true` for a subscribe, `false` for an unsubscribe |
| `SubscriptionEntry(String filter, boolean pattern)` | record | one active subscription |
| | `filterBytes()` | `filter` UTF-8 encoded |
| | `fromBytes(byte[], boolean)` | static factory decoding `filter` from UTF-8 bytes |

**Completion result.** Both are plain data holders with no async behavior; `SubscriptionEvent` has
no `close()` — it owns no native resources.

**When to use.** On an XPUB socket's subscription-event receive path (Sockets category) to observe
subscriber churn. `SubscriptionEntry` is the return type of a socket's subscription-snapshot
lookup (Sockets category).

---

## Send / request / reply operation-builder shape

The fluent builder every socket type's `send`/`publish`/`request`/`reply` entry point (Sockets
category) returns to accumulate parts, flags, and a terminal submit. All builder interfaces extend
the shared `MessageBuilderStage<TSubmit>` (`TSubmit message(Message part)`), and the request
family additionally extends `TimeoutSubmitOperation<TResult, TCallback>`.

```java
dealer.send().message(part1).message(part2).submit();

CompletionStage<List<Message>> future = dealer.request()
    .message(Message.from("payload"))
    .timeout(Duration.ofSeconds(5))
    .submit();
List<Message> reply = future.toCompletableFuture().join();

// or, on a virtual thread:
List<Message> reply2 = dealer.request().message(Message.from("payload")).await();

received.reply().message(Message.from("ok")).submit();
```

**Options.**

| Stage | Member | Meaning |
| --- | --- | --- |
| `SendOperation` | `.message(Message)` | starts the chain |
| `SendSubmitOperation` | `.message(...)` / `.flags(SendFlags)` / `.submit()` | add parts, set flags, terminal |
| `RequestOperation`/`RequestSubmitOperation` | same as `Send` + `.timeout(Duration)` | mirrors the send chain, adding a reply-wait timeout |
| `RequestSubmitOperation.flags(SendFlags)` | narrows to `RequestCallbackSubmitOperation` | drops the `CompletionStage`-returning `.submit()` — only `.submit(RequestCallback)` remains reachable |
| `ReplyOperation`/`ReplySubmitOperation` | mirrors `Send` | no flags stage — the underlying reply function takes no send-flag argument |
| `TimeoutSubmitOperation.await()` | `default` method | submits and blocks the current thread until the result completes; explicitly intended for virtual threads (parking a virtual thread frees its carrier platform thread, unlike blocking a platform thread directly) — the framework's own async path uses `submit()` instead |

**Completion result.**

| Terminal | Returns | Meaning |
| --- | --- | --- |
| `SendSubmitOperation.submit()` | `boolean` | `false` only when `SendFlags.DONT_WAIT` is set and the send would have blocked; other failures throw `ZlinkException` |
| `ReplySubmitOperation.submit()` | `void` | throws `ZlinkException` on failure |
| `RequestSubmitOperation.submit()` | `CompletionStage<List<Message>>` | the caller owns and must close the reply messages |
| `submit(RequestCallback)` (`RequestSubmitOperation`/`RequestCallbackSubmitOperation`) | `boolean` | same `DONT_WAIT` convention; delivers the result and parts to the callback later — the callback owns the parts only when the result is `RequestResult.OK` |

Every builder consumes its accumulated `Message` parts on a successful submit only; on failure
ownership is restored to the caller.

**When to use.** `submit()`'s `CompletionStage` in ordinary async code; `await()` instead on a
virtual thread for code that reads more naturally as a sequential call; `.flags(...).submit(callback)`
when a callback-completion surface is needed on a thread that shouldn't block or park at all.
`Received.reply()`/`send()` rather than reconstructing the destination route by hand.

---

See [`contracts/messaging/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/messaging/)
and the [Java binding spec](../../spec/java/README.en.md) for the full rationale.
