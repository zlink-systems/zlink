[한국어](multipart-atomicity.ko.md)

# Multipart Atomicity

> **The document that owns this chapter's contract** — the public contract
> for multipart framing is covered by the
> [message API reference](../spec/core/02-message.en.md). This chapter
> explains how the internals protect against another sender's parts getting
> mixed in.

Core treats the parts from `ZLINK_PART_MORE` through `ZLINK_PART_FINAL` as
one logical multipart sequence. Per-socket transaction state protects the
send path so another sender's message parts can't be inserted into the
middle of this sequence.

## Send

The first part starts the transaction, and the final part commits it. If a
send fails partway through, it does not consume the remaining
caller-owned parts, and it cleans up the internal transaction state so the
next message doesn't pick up the previous sequence. Depending on the
return value, the caller either closes or reuses the part it still owns.

## Receive

The typed receive API returns one part plus either `ZLINK_PART_MORE` or
`ZLINK_PART_FINAL`. The receive helper confirms the same handle, socket
family, source, and owner thread stay unchanged during the sequence. If
the sequence is interrupted, it closes the buffered part and resets the
helper state.

## Request/Reply

The request control part and the application payload are sent in one
transaction. The receive path validates and strips the control part, then
returns the request sequence and peer routing id as typed metadata.

## Concurrency

Multiple threads can send independent messages, but a single multipart
message must not be split across threads. Receive follows a
single-consumer contract.
