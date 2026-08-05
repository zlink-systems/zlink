[한국어](02-messaging.ko.md) | English

[Reference index](README.en.md)

# 02. Messaging

This category covers message ownership and the receive envelope types (`Received`,
`ReceivedMessage`, `TopicMessage`, `SubscriptionEvent`). **The send/request/reply operation-builder
family is not declared here** — unlike every other language covered so far, those Protocols
(`SendOp`, `RequestOp`, `RequestCallbackOp`, `ReplyOp`) live in `contracts/sockets/operations.py`
and are documented in the Sockets category instead. The exact signatures are owned by
[`contracts/messaging/`](../../../../bindings/python/src/zlink/contracts/messaging/).

---

## `Message`

A message payload. Supports both sync and async context-manager protocols; closing it (or leaving
its `with`/`async with` block) releases the payload. Sending a message consumes it.

```python
sized = Message.allocate(4096)
copy = Message.from_(b"payload")
with copy:
    view = copy.data
```

**Options.**

| Member | Meaning |
| --- | --- |
| `from_(data)` | class method; an independent copy of any bytes-like `data`; named with a trailing underscore since `from` is a keyword |
| `allocate(size: int)` | class method; writable storage |
| `copy()` | independent payload copy |
| `size()` | payload length in bytes |
| `is_empty()` | whether `size()` is zero |
| `data` | property, a zero-copy `memoryview`, valid only while the message is open |
| `to_bytes()` | copy of the payload as `bytes` |
| `copy_to(destination, source_offset=0, destination_offset=0, length=None)` | copies the payload (or a range) into a caller-provided buffer, returns bytes written |
| `try_copy_to(destination)` | returns bytes written, or `None` if `destination` is too small — the non-raising alternative to `copy_to` |
| `to_string(encoding="utf-8")` | payload decoded as text |
| `ref_count()` | native reference count, diagnostic only |
| `close()` | releases the message |

**Completion result.** All members are synchronous. `Message` supports both `with`/`async with`.

**When to use.** Use `Message.allocate(size)` or a copying `Message.from_(data)` to build an
outbound payload. Prefer the zero-copy `data` `memoryview` over `to_bytes()` when an independent
copy isn't needed — but only while the message stays open, since the view aliases the message's
own storage.

---

## `ReceivedMessage`

A single received message part, distinct from `Message` — the type yielded when iterating a
`ReceivedMultipart`/`Received` envelope.

**Options.** No parameters — obtained only by iterating an envelope, never constructed directly.

| Member | Meaning |
| --- | --- |
| `__len__` | part size in bytes, via `len(part)` |
| `data` | property, a `memoryview` snapshot |
| `to_bytes()` | copy of the part as `bytes` |
| `close()` | releases the part |

**Completion result.** Synchronous; supports both sync/async context-manager protocols.

**When to use.** Iterate a `Received`/`TopicMessage` envelope (`for part in received:`) to access
each `ReceivedMessage` in order, rather than indexing into a separate parts list.

---

## `ReceivedMultipart` / `Received`

`ReceivedMultipart` is the shared multipart-envelope shape (iterate or index its parts);
`Received` extends it with routing metadata and an optional reply/send context. Both own their
parts until closed.

```python
received = create_received()
if dealer.recv_into(received):
    if received.is_single_part():
        pass
    received.reply().message(b"ok").submit()
```

**Options.** **Neither `Received` nor `ReceivedMultipart` exposes `routing_id`/`request_seq` as a
documented public member in this contract** — reply/send context is reached only through the
`reply()`/`send()` methods themselves.

| Type | Member | Meaning |
| --- | --- | --- |
| `ReceivedMultipart` | `__iter__` | iterates the parts in order |
| | `__len__` | part count, via `len(envelope)` |
| | `to_bytes_list()` | list of `bytes` copies, one per part |
| | `is_single_part()` | whether the envelope has exactly one part |
| | `first_part()` | the first part, without transferring ownership; raises `RecvError` when empty |
| | `single_part_or_throw()` | the single part; raises `RecvError` unless exactly one part |
| | `close()` | closes every owned part |
| `Received extends ReceivedMultipart` | `send()` | starts the shared `SendOp` builder (Sockets category), addressed to this envelope's captured source route; raises `SubmitError` if the envelope carries no send context |
| | `reply()` | starts the shared `ReplyOp` builder; raises `SubmitError` unless the envelope is replyable |

**Completion result.** All members are synchronous. Both support sync/async context-manager
protocols.

**When to use.** Reuse one `Received` (via `create_received()`) across a receive loop rather than
constructing a new one per message. Check whether `reply()`/`send()` raises before assuming an
envelope is replyable/has a send context, since there is no separate boolean/property to test for
it ahead of time.

---

## `TopicMessage`

A received publish: its topic and message parts. Extends `_BaseReceived` (the same shared shape
`ReceivedMultipart` is built from) rather than `ReceivedMultipart` itself. Owns its parts until
closed.

```python
published = create_topic_message()
if sub.subscribe_into(published):
    topic = published.topic
```

**Options.**

| Member | Meaning |
| --- | --- |
| `topic` | property, get **and set** — unlike every other language's read-only `topic`; the topic this publish was sent on |
| `__iter__` / `__len__` / `to_bytes_list()` / `is_single_part()` / `first_part()` / `single_part_or_throw()` / `close()` | same shape as `ReceivedMultipart` |

**Completion result.** Synchronous; supports sync/async context-manager protocols.

**When to use.** Reuse one instance (via `create_topic_message()`) across a subscribe-receive loop
the same way as `Received`.

---

## `SubscriptionEvent`

Reports one subscriber's subscribe or unsubscribe, as observed by an XPUB socket.

```python
evt = create_subscription_event()
if xpub.receive_subscription_event_into(evt):
    ...
```

**Options.** This `Protocol` declares no members of its own beyond its docstring description
("its routing id, topic, and whether it subscribed") — the concrete runtime implementation exposes
`routing_id`/`topic`/`subscribed`-shaped attributes, but this contract file itself declares none as
typed members, unlike `Message`/`Received`/`TopicMessage` above, which enumerate their surface
explicitly.

**Completion result.** N/A — no members are contractually specified here.

**When to use.** Use on an XPUB socket's subscription-event receive path (Sockets category) to
observe subscriber churn; consult the runtime implementation or the spec directly for the exact
attribute names, since this Protocol doesn't enumerate them.

---

See [`contracts/messaging/`](../../../../bindings/python/src/zlink/contracts/messaging/) and the
[Python binding spec](../../spec/python/README.en.md) for the full rationale.
