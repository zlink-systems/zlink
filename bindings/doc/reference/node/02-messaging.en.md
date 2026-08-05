[한국어](02-messaging.ko.md) | English

[Reference index](README.en.md)

# 02. Messaging

This category covers message ownership, the receive envelope types (`Received`, `TopicMessage`,
`SubscriptionEvent`), and the shared send/request/reply operation-builder family every socket
type's entry point returns. The exact signatures are owned by
[`contracts/messaging/`](../../../../bindings/node/src/zlink/contracts/messaging/).

---

## `Message`

Owns one message payload. A `Message` created via `Message.from(...)`/`Message.allocate(...)` is
an immutable value copy — it is frozen and does not need explicit release; only a runtime-received
message owns native storage that `close()` actually releases.

```ts
const sized = Message.allocate(4096);
const copy = Message.from('payload');
const copyOfBuffer = Message.from(rawBuffer);
```

**Options.**

| Member | Meaning |
| --- | --- |
| `Message.from(buffer: BufferLike \| Message)` | a `string` is UTF-8 encoded, a `Buffer`/`Uint8Array` is copied, another `Message` is deep-copied |
| `Message.allocate(size: number)` | writable storage; throws `RangeError` on a negative or unsafe-integer size |
| `data()` | returns a `Buffer` backed by this message's storage |
| `toBytes()` | independent copy of the payload |
| `copy()` | equivalent to `Message.from(this)` |
| `size()` | payload length in bytes |
| `isEmpty()` | whether `size()` is zero |
| `copyTo(destination, sourceOffset?, destinationOffset?, length?)` | copies the payload (or a range) into a caller-provided buffer, returns bytes written; throws `RangeError` out of range |
| `tryCopyTo(destination)` | bounds-checked variant, returns `boolean` instead of throwing on a too-small destination |
| `getString(encoding = 'utf8')` / `toString()` | payload decoded as text; `toString()` is equivalent to `getString()` |
| `refCount()` | native reference count, diagnostic only |
| `getProperty(name)` | returns `string \| null` — **native message metadata is reserved but not populated yet**, so this always returns `null` today |
| `close()` | releases the message |

**Completion result.** Every member is synchronous. `close()` on a frozen (factory-created) message
is a no-op; on a runtime-received message it releases native storage, resetting the instance to an
empty state.

**When to use.** Use `Message.allocate(size)` or a copying `Message.from(...)` to build an outbound
payload from data the caller doesn't need to keep raw ownership of. Use `tryCopyTo` over `copyTo`
when the destination size isn't already known to be sufficient. Treat `getProperty(...)` as
currently non-functional — it is reserved surface, not a working metadata lookup.

---

## `MessagePartsEnvelope` shared base

The exported abstract base both `Received` and `TopicMessage` extend — a Node-specific public base
class (unlike java's/cpp's non-public equivalents).

**Options.**

| Member | Meaning |
| --- | --- |
| `parts` | `Message[]`, owned by the envelope |
| `isSinglePart()` | whether `parts` has exactly one element |
| `firstPart()` | the first part, without transferring ownership; throws when the envelope has no parts |
| `singlePartOrThrow()` | the single part; throws unless exactly one part |
| `close()` | closes every part |

**Completion result.** All members are synchronous.

**When to use.** Not constructed directly — use `Received`/`TopicMessage` below, both of which
inherit this shape.

---

## `Received`

A received message envelope: routing metadata, message parts, and an optional reply/send context.
Owns its parts until closed; reuse one instance across `recv` calls to avoid a per-receive
allocation.

```ts
const received = new Received();
if (dealer.recv(received)) {
  if (received.requestSeq !== null) {
    received.reply().message(Message.from('ok')).submit();
  }
}
```

**Options.** Constructor takes no arguments (throws `TypeError` if called with any). Extends
`MessagePartsEnvelope`.

| Member | Meaning |
| --- | --- |
| `routingId` | `RoutingId \| null`, present when the receive path provides one |
| `requestSeq` | `bigint \| null`, present when replyable |
| `reply()` | starts the shared `ReplyOperation` builder; throws `SubmitError` when the envelope has no request sequence/reply context |
| `send()` | starts the shared `SendOperation` builder, addressed to this envelope's captured source route; throws `SubmitError` when the envelope carries no send context |

**Completion result.** All members are synchronous.

**When to use.** Reuse one `Received` across a receive loop rather than constructing a new one per
message. Check `requestSeq !== null` before calling `reply()` to confirm the envelope is replyable.

---

## `TopicMessage`

A received publish: its topic and message parts. Owns its parts until closed.

```ts
const published = new TopicMessage();
if (sub.subscribe(published)) {
  const topic = published.topic;
}
```

**Options.** No-argument constructor. Extends `MessagePartsEnvelope`.

| Member | Meaning |
| --- | --- |
| `routingId` | `RoutingId \| null`, the publisher's routing id, present when the receive path provides one |
| `topic` | `string`, the topic this publish was sent on — a plain mutable field rather than a getter |

**Completion result.** Synchronous.

**When to use.** Reuse one instance across a subscribe-receive loop the same way as `Received`.

---

## `SubscriptionEvent` / `SubscriptionEntry`

Reports one subscriber's subscribe or unsubscribe (as observed by an XPUB socket), and describes
one active subscription entry.

```ts
const evt = new SubscriptionEvent();
if (xpub.receiveSubscriptionEvent(evt)) { /* ... */ }
```

**Options.** `SubscriptionEvent` is a plain class with a no-argument constructor and mutable
fields; `SubscriptionEntry` is a plain interface (not a class) with `readonly` fields.

| Type | Member | Meaning |
| --- | --- | --- |
| `SubscriptionEvent` | `routingId` (`RoutingId \| null`) | the subscriber's routing id, present when the receive path provides one |
| | `topic` (`string`) | the topic that was subscribed or unsubscribed |
| | `subscribed` (`boolean`) | `true` for a subscribe, `false` for an unsubscribe |
| `SubscriptionEntry` | `readonly filter: string` | the subscription filter text |
| | `readonly isPattern: boolean` | whether `filter` is a pattern match rather than a literal prefix |

**Completion result.** Both are plain data holders with no async behavior; `SubscriptionEvent` has
no `close()` — it owns no native resources.

**When to use.** Use on an XPUB socket's subscription-event receive path (Sockets category) to
observe subscriber churn. `SubscriptionEntry` is the return type of a socket's
subscription-snapshot lookup (Sockets category).

---

## Send / request / reply operation-builder shape

The fluent builder every socket type's `send`/`publish`/`request`/`reply` entry point (Sockets
category) returns to accumulate parts, flags, and a terminal submit. Every builder stage extends
one of three small generic stage interfaces: `PartBuilder<TNext>` (`message(m): TNext`),
`Flaggable<TNext>` (`flags(f): TNext`), `Timeoutable<TNext>` (`timeout(ms): TNext`).

```ts
dealer.send().message(Message.from('p1')).message(Message.from('p2')).submit();

const reply = await dealer.request()
  .message(Message.from('payload'))
  .timeout(5000)
  .submit();

received.reply().message(Message.from('ok')).submit();
```

**Options.**

| Stage | Member | Meaning |
| --- | --- | --- |
| `SendOperation` | `extends PartBuilder<SendSubmitOperation>` | `.message(m)` starts the chain |
| `SendSubmitOperation` | `.message(...)` / `.flags(f)` / `submit(): boolean` | add parts, set flags, terminal |
| `RequestOperation` | `extends PartBuilder<RequestSubmitOperation>` | `.message(m)` starts the chain |
| `RequestSubmitOperation` | `.message(...)` / `.timeout(timeoutMs: number)` | mirrors the send chain, adding a reply-wait timeout in plain milliseconds (not a Duration-like type) |
| `RequestSubmitOperation.flags(f)` | narrows to `RequestCallbackSubmitOperation` | drops the `Promise`-returning `submit()` — only `submit(callback: RequestCallback): boolean` remains reachable |
| `RequestSubmitOperation.submit()` | no-argument overload | returns `Promise<Message[]>` |
| `ReplyOperation` | `extends PartBuilder<ReplySubmitOperation>` | `.message(m)` starts the chain |
| `ReplySubmitOperation` | `.message(...)` / `.flags(f)` / `submit(): void` | mirrors `Send`, no timeout stage |

**Completion result.** `SendSubmitOperation.submit()` returns `boolean` synchronously (`false` only
when `SendFlags.DontWait` is set and the send would have blocked — other failures throw a typed
error, Errors category). `ReplySubmitOperation.submit()` returns `void`.
`RequestSubmitOperation.submit()` (no-argument form) returns `Promise<Message[]>` — the caller owns
and must close the reply messages. `RequestSubmitOperation.submit(callback)`/
`RequestCallbackSubmitOperation.submit(callback)` return `boolean` (same `DontWait` convention) and
deliver `(result, parts)` to `RequestCallback` later — the callback owns `parts` only when `result`
is the success value. Every builder consumes its accumulated `Message` parts on a successful submit
only; on failure ownership is restored to the caller.

**When to use.** Use the no-argument `submit()`'s `Promise` in ordinary `async`/`await` code; use
`.flags(...).submit(callback)` when a callback-completion surface is needed instead of a promise.
Use `Received.reply()`/`send()` rather than reconstructing the destination route by hand.

---

## Handler type aliases

Function-type aliases used as callback parameters across the Sockets/Eventing categories.

| Alias | Registered by | Signature |
|---|---|---|
| `SocketSendReadyHandler` | A socket's send-ready registration (Sockets category) | `() => void` |
| `StreamPacketHandler` | `StreamSocket`'s packet registration (Sockets category) | `(sourceRid: RoutingId, header: Message, body: Message) => void` — owns both messages |
| `SocketMonitorHandler` | `SocketMonitor.onEvent(...)` (Eventing category) | `(event: MonitorEvent) => void` |
| `RequestCallback` | `RequestSubmitOperation.submit(callback)`/`RequestCallbackSubmitOperation.submit(callback)` (above) | `(result: RequestResult, parts: readonly Message[]) => void` — owns `parts` only on success |
| `ReplyHandler` | Not currently reached by any documented public entry point in this reference tier | `(result: RequestResult, parts: Message[]) => void` |

---

See [`contracts/messaging/`](../../../../bindings/node/src/zlink/contracts/messaging/) and the
[Node binding spec](../../spec/node/README.en.md) for the full rationale.
