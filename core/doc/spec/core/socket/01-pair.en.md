---
title: "Socket — PAIR"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/socket/01-pair/) | English

<!-- zlink-nav:start -->
[Socket Index](README.en.md) | [Previous: Socket Overview](README.en.md) | [Next: PUB](02-pub.en.md)
<!-- zlink-nav:end -->

# Socket — PAIR

> **What this chapter defines** — The exclusive 1:1 connection behavior and public contract of a PAIR socket.

## 1. PAIR overview

PAIR is a bidirectional socket type in which two [socket](../glossary.en.md#socket) instances form an exclusive
1:1 connection and both sides send and receive messages. Because there is exactly one peer—the socket
at the other end of the connection—there is no input for selecting a destination peer, and a received
part has no source routing ID. PAIR has no type-specific options.

This document defines only the PAIR-specific contract: how the part send and receive functions behave
with PAIR, the asynchronous send rules for admission—the decision to accept a send request into the
Core send queue—and the absence of receive-flow state. It does not redefine contracts shared by all
socket types.

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| socket creation, common options, send/recv flags, and result enums | [Socket Common](README.en.md) |
| ownership transfer, pending bound, timeout, cancellation, close fail-fast, and callback rules for asynchronous sends | [Socket Common](README.en.md) |
| message lifecycle, ownership, and multipart | [Message](../02-message.en.md) |
| result-to-errno mapping | [Errors](../03-errors.en.md#result-and-errno-mapping) |

## 2. Multipart sends and record atomicity

A PAIR socket submits a message one part at a time. Send a single-part message with
`ZLINK_PART_FINAL`. A [multipart](../02-message.en.md#4-multipart) message groups multiple parts into
one logical message: start it with `ZLINK_PART_MORE`, then continue through `ZLINK_PART_FINAL` on the
same thread, using the same function and the same `flags_`.

Core stages successful intermediate parts as one group until `ZLINK_PART_FINAL` succeeds. This group
is called a record. If any intermediate or final submit in an open sequence fails, Core atomically
discards the previously staged parts and the failed part, then closes the sequence. The peer sees no
part of that record.

```mermaid
sequenceDiagram
    participant App as Application
    participant Core as Core
    App->>Core: zlink_send_part(part 1, ZLINK_PART_MORE)
    Note over Core: Stage the successful intermediate part in the record
    App->>Core: zlink_send_part(part 2, ZLINK_PART_MORE)
    alt All submits, including the final submit, succeed
        App->>Core: zlink_send_part(part 3, ZLINK_PART_FINAL)
        Note over Core: The record is complete
    else An intermediate or final submit fails
        App--xCore: zlink_send_part(part N, ...) fails
        Note over Core: Atomically discard the staged parts and failed part,<br/>then close the sequence<br/>The peer sees no part of the record
    end
```

The failed call also consumes its `part_` according to the consumption rules of
[`zlink_send_part`](#zlink_send_part), and the next submit starts the first part of a new record. A retry
therefore must resubmit the entire record from its first part using copies retained before the calls.

## 3. Receive flow state

[Socket Common](README.en.md) defines the receive-flow state and constants that DEALER and ROUTER
sockets use to notify a peer to pause or resume receiving over the
[completion lane](../glossary.en.md#completion-progress-lane).

PAIR has no paired DEALER/ROUTER completion lane, so it has no receive-flow state.
`zlink_socket_set_receive_flow_state()` returns `ZLINK_CONFIG_NOT_SUPPORTED` with `errno == ENOTSUP`
for a PAIR socket and changes nothing. The byte [HWM](../glossary.en.md#hwm) (the byte limit retained
by a queue), low water mark, and transport [backpressure](../glossary.en.md#backpressure) (restriction
on additional submissions by the sender) owned by [Socket Common](README.en.md) remain in effect. A
PAIR socket's monitor does not set `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` and does not emit
`ZLINK_EVENT_SEND_FLOW_PAUSED`, `ZLINK_EVENT_SEND_FLOW_RESUMED`, or
`ZLINK_EVENT_FLOW_STATE_STALE`.

## 4. Functions

### zlink_send_part

Sends one message part.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part (
  void *s_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

[Section 2](#2-multipart-sends-and-record-atomicity) defines single-part sends with
`ZLINK_PART_FINAL`, the rules for starting and continuing a multipart message, and record-level
atomicity.

This function consumes the content of `part_` on both success and failure. If the same content may be
needed again, copy it before the call. Initialize a consumed `zlink_msg_t` before reusing it. Pass
`ZLINK_SEND_FLAGS_NONE` or `ZLINK_DONTWAIT` in `flags_`. A non-blocking call that cannot proceed
immediately returns `ZLINK_SUBMIT_BACKPRESSURED`.

**Returns:** `ZLINK_SUBMIT_OK` on success; otherwise a `zlink_submit_result_t` value that identifies
the cause. The full mapping follows the [errno map](../03-errors.en.md#result-and-errno-mapping).

**See also:** `zlink_recv_part`, `zlink_send_async`

---

### zlink_recv_part

Receives one message part.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (
  void *s_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

`part_out_` must be an initialized message and is required together with `has_more_out_`.
`source_rid_out_` is optional and, when provided, receives `NULL` on success for PAIR. On success,
ownership of the received part transfers to the caller, which must call `zlink_msg_close(part_out_)`
exactly once. A failure before a part is received does not transfer ownership.

`*has_more_out_` is `ZLINK_PART_MORE` when another part follows and `ZLINK_PART_FINAL` for the last
part. Receive all parts of one multipart message on the same thread with this function, from the first
part through the last part. The normal path is to call it after observing `ZLINK_POLLIN` with a poller.
A `ZLINK_DONTWAIT` call with no available data returns `ZLINK_RECV_NO_DATA`.

**Returns:** `ZLINK_RECV_OK` on success; otherwise a `zlink_recv_result_t` value.

**See also:** `zlink_send_part`, `zlink_msg_close`

---

### Asynchronous send admission

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_async (
  void *s_, zlink_msg_t *parts_, size_t part_count_,
  const zlink_send_async_options_t *options_,
  zlink_send_op_id_t *op_id_out_);

ZLINK_EXPORT zlink_handler_result_t zlink_send_complete_handler (
  void *s_, zlink_send_complete_handler_fn handler_, void *userdata_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_async_cancel (
  void *s_, zlink_send_op_id_t op_id_);
```

PAIR supports the complete asynchronous send admission surface. `options_->target` is ignored. A PAIR
socket has exactly one peer, so all pending operations share one target queue and are admitted in
submit order.

Completion means admission into the Core send queue, not delivery to the peer. [Socket
Common](README.en.md) owns the complete contract, including ownership transfer, the per-socket pending
bound, per-operation timeout, cancellation, close fail-fast, and callback rules.

**Returns:** `ZLINK_SUBMIT_OK` / `ZLINK_HANDLER_OK` on success; otherwise a
`zlink_submit_result_t` or `zlink_handler_result_t` value.

**See also:** `zlink_send_part`

## 5. Implementation and contract-test verification requirements

Verify the following using only the public surface (`zlink_send_part`, `zlink_recv_part`, the
`zlink_send_async` family, `zlink_socket_set_receive_flow_state`, monitor observations, return values,
and errno). Each item maps to one test.

**1:1 send and receive**
- Both connected PAIR sockets can send with `zlink_send_part` and receive with `zlink_recv_part`.
- When `zlink_recv_part` succeeds, a caller that provides `source_rid_out_` receives `NULL`.
- After a successful receive, the caller owns the part and calls `zlink_msg_close(part_out_)` exactly once. A failure before a part is received does not transfer ownership.

**Part flow**
- When a single-part message is sent with `ZLINK_PART_FINAL`, the receiver observes `ZLINK_PART_FINAL` in `*has_more_out_`.
- For a multipart message, the receiver observes `ZLINK_PART_MORE` on every part before the last and `ZLINK_PART_FINAL` on the last part.
- A `ZLINK_DONTWAIT` send that cannot proceed immediately returns `ZLINK_SUBMIT_BACKPRESSURED`; a `ZLINK_DONTWAIT` receive with no available data returns `ZLINK_RECV_NO_DATA`.

**Record atomicity**
- If any submit in an open multipart sequence fails, the peer receives no part of that record.
- Both successful and failed calls consume `part_`, including the failed call's `part_`; a consumed `zlink_msg_t` can be reused only after it is initialized again.
- The next submit after a failure starts the first part of a new record—the entire record retained before the calls can be resubmitted from its first part for a retry.

**Asynchronous send admission**
- The complete `zlink_send_async`, `zlink_send_complete_handler`, and `zlink_send_async_cancel` surface works with PAIR.
- A submitted value in `options_->target` is ignored.
- Multiple pending operations are admitted in submit order.
- A completion notification means admission into the Core send queue, not confirmation of delivery to the peer.

**Absence of receive flow state**
- `zlink_socket_set_receive_flow_state()` returns `ZLINK_CONFIG_NOT_SUPPORTED` with `errno == ENOTSUP` for a PAIR socket, while byte HWM, low water mark, and transport backpressure behavior remain in effect.
- A PAIR socket's monitor status does not set `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE`.
- A PAIR socket does not emit `ZLINK_EVENT_SEND_FLOW_PAUSED`, `ZLINK_EVENT_SEND_FLOW_RESUMED`, or `ZLINK_EVENT_FLOW_STATE_STALE` events.

[Socket Common](README.en.md) owns verification of ownership transfer, the pending bound,
per-operation timeout, cancellation, close fail-fast, and callback rules.
