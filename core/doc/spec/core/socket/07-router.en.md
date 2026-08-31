---
title: "Socket — ROUTER"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/socket/07-router/) | English

<!-- zlink-nav:start -->
[Socket Index](README.en.md) | [Previous: DEALER](06-dealer.en.md) | [Next: STREAM](08-stream.en.md)
<!-- zlink-nav:end -->

# Socket — ROUTER

> **What this chapter defines** — The public contract for routing replies by routing ID on a
> ROUTER socket and for [result/errno](../03-errors.en.md#result-and-errno-mapping).

## 1. ROUTER overview

ROUTER is an asynchronous raw socket that manages connections (pipes) to multiple peers on one
[socket](../glossary.en.md#socket) and selects a send target by routing ID, the byte sequence that
identifies a peer. It processes ordinary directed messages and received request records. The
[Message](../02-message.en.md#zlink_routing_id_t) specification owns the contract for
`zlink_routing_id_t`, the type that carries a routing ID.

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| Socket creation, common options, and the `zlink_socket_set_receive_flow_state` declaration | [Socket Common](README.en.md) |
| Routing ID type (`zlink_routing_id_t`) | [Message](../02-message.en.md#zlink_routing_id_t) |
| Request-reply kinds, sequences, and ZMP header byte layout and validation | [ZMP](../protocol/01-zmp.en.md) |
| Mapping of each result to errno and the receive flow state result table | [Errors](../03-errors.en.md) |
| Peer socket paired with ROUTER | [DEALER](06-dealer.en.md) |
| Socket status snapshot | [Monitoring](../06-monitoring.en.md) |

## 2. Raw receive record classification

The ROUTER receive API does not return the `zlink_dealer_message_type_t` from
[DEALER](06-dealer.en.md). The following output combinations distinguish an ordinary raw record
from a request record that requires a reply.

| Record | `source_node_rid_out_` | `request_seq_out_` |
|---|---|---:|
| Ordinary raw multipart | Sending peer's routing ID | `0` |
| Received request | Sending peer's routing ID | Nonzero opaque reply token |

Replies and terminal failures for a request started with `zlink_router_request_part()` are not
returned as receive records. They are delivered only through the `zlink_reply_handler_fn`
completion.

Core preserves the request kind and wire sequence decoded from the ZMP header as internal values on
the first payload part. `zlink_router_recv_part()` and `_v2()` interpret only a request kind as a
reply-capable record and store its source routing ID, exact source pipe, pair identity, and original
wire sequence as a reply target. A nonzero `request_seq_out_` is a socket-local opaque reply token
used with the source routing ID, not the wire sequence itself. Without a collision, Core may return
the wire-sequence value as the token. When different physical sources with the same routing ID send
the same live wire sequence, Core returns an alias token for one of them. If the same physical source
reuses a live wire sequence, Core terminates that pair with `EPROTO`.

The application uses the returned RID and token unchanged for a reply. Core finds the saved target
by `(source RID, token)`, re-stamps the original wire sequence into the reply header, and sends the
reply through the exact source pipe and pair that delivered the request. Thus duplicate or standby
connections with the same RID, reverse replies, and out-of-order replies cannot complete a request
from another physical source.

A reply or error reply received through a typed surface terminates the pair with `EPROTO` and
returns no payload. Before `zlink_router_recv_part()` or `_v2()` hands a data or request payload to
the application, it removes the internal kind and sequence. Raw-sending that part again therefore
produces ordinary data. The common `zlink_recv_part()` surface does not support ROUTER receive.

`source_node_rid_out_` is a thread-local view owned by Core. The caller does not release it and
copies the value if it must be retained after the next raw receive call. Starting the next receive
call on the same thread invalidates the previously returned view. Every part of one multipart
record returns the same routing ID and opaque reply token.

## 3. Part sequences and ownership

`*_part` send calls form one multipart sequence from `ZLINK_PART_MORE` through
`ZLINK_PART_FINAL`. While a sequence is open, another send helper family or a different routing ID
cannot be interleaved on the same handle.

When a valid initialized `part_` is passed to a send API, the function consumes its message content
on both success and failure and leaves it as an initialized zero-length message. The caller
therefore cannot read the pre-submit payload or send the same content again after the call,
regardless of the result. Payload needed for another send must be retained in a separate message
before the call.

Each send helper family stages successful intermediate parts as one record until
`ZLINK_PART_FINAL` succeeds. If an intermediate or final submit in an open sequence fails, Core
atomically discards the previously staged parts and the failed part and closes the sequence. No
part of that record becomes visible to the peer. The failed call also consumes `part_`, and the next
submit starts the first part of a new record. A failed request submit creates no request sequence
and invokes no handler. If a reply sequence fails, the reply-token and peer-RID pair remains valid
until a successful `ZLINK_PART_FINAL` or the end of the request lifecycle, so the caller can
resubmit a retained complete reply from its first part.

If the first application part of a request or reply has a non-empty message group, Core cannot store
the internal request metadata in the same area. It rejects the entire submission with
`ZLINK_SUBMIT_INVALID_ARGUMENT` and `EINVAL`. The supplied C part follows the same consumption rule,
and no staged part, pending request, or reply target remains.

The `part_out_` passed to a receive API must be an initialized `zlink_msg_t` before the call. On
success, ownership of the received part moves to the caller, which releases it exactly once with
`zlink_msg_close()`. On failure, ownership of the received part does not move.

## 4. Public types

The following numbers are public ABI values.

```c
typedef enum zlink_router_option_t {
  ZLINK_ROUTER_OPT_MANDATORY          = 0x3101,  // int, 0=off, positive=on, getter returns 0/1, default 1. Whether directed submit to an unconnected routing ID fails
  ZLINK_ROUTER_OPT_PROBE              = 0x3103,  // int, 0=off, positive=on, getter returns 0/1, default 0. An empty raw message lets the peer observe the connection and routing ID when the connection is established
  ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID = 0x3104,  // Variable-length byte string, set only. Local alias for the next zlink_connect() pipe
  ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS = 0x3105,  // Nonnegative int (milliseconds), default 5000. Default timeout when a request uses timeout_ms_ == 0
  ZLINK_ROUTER_OPT_WEIGHT             = 0x3106   // int, 0..10000, default 100. This ROUTER's weight advertised to connected peers
} zlink_router_option_t;

typedef enum zlink_part_flag_t {
  ZLINK_PART_FINAL = 0,  // The current part is the last part of the record
  ZLINK_PART_MORE  = 1   // Another part follows in the same multipart record
} zlink_part_flag_t;

typedef void (*zlink_reply_handler_fn)(
  zlink_request_result_t result_,
  zlink_msg_t *parts_,
  size_t part_count_,
  void *userdata_);
```

`ZLINK_PART_MORE` means that another part follows in the same multipart record.
`ZLINK_PART_FINAL` means that the current part is the last part. Receive APIs use the same values
for `has_more_out_`.

## 5. ROUTER options

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_router_option(
  void *handle_,
  zlink_router_option_t option_,
  const void *optval_,
  size_t optvallen_);

ZLINK_EXPORT zlink_config_result_t zlink_get_router_option(
  void *handle_,
  zlink_router_option_t option_,
  void *optval_,
  size_t *optvallen_);
```

The inline comments in [section 4](#4-public-types) define the value format, range, and default for
each option. The following contracts are not included in those comments.

- When `ZLINK_ROUTER_OPT_MANDATORY` is positive, a directed submit to a routing ID without a
  connected pipe fails with `ZLINK_SUBMIT_NOT_CONNECTED`.
- `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID` sets the local alias that identifies the pipe created by
  the next `zlink_connect()` and is set before each connect.

When `zlink_get_router_option()` is called, `*optvallen_` is the input capacity of `optval_`. On
success, it is updated to the number of bytes written. [HWM](../glossary.en.md#hwm), the byte limit
for queue storage, and reconnect and timeout options that are not ROUTER-specific use
`zlink_set_option()` and `zlink_get_option()`.

`ZLINK_ROUTER_OPT_WEIGHT` is the absolute value that a peer uses when selecting this ROUTER as an
outbound candidate. ROUTER and DEALER advertise their own values independently, so each direction
uses the value advertised by the other socket.

The public weight result follows this order.

1. A value set before bind or connect applies after the paired Application pipe becomes ready.
2. A dynamic change applies the new absolute value, including `0`, to the peer scheduler.
3. An actual change emits `PEER_WEIGHT_CHANGED` with the value and the paired Application pipe's
   lane, pair ID, and generation. Repeating the same value emits no additional event.
4. Reconnect applies the current configured value to the new generation.

The network wire, inproc delivery, CONTROL size boundary, multipart deferral, and exact-pipe
lifetime and stale-delivery ownership are defined by the
[ZMP transport-pair](../protocol/01-zmp.en.md#41-request-reply-transport-pair),
[decode](../protocol/01-zmp.en.md#7-decode-validation), and
[peer-weight owner](../protocol/01-zmp.en.md#peer-weight-control) contracts. Neither transport path
creates a weight record on public receive or the Completion lane.

If the applied value becomes `0` after a multipart has selected a pipe, that message completes
through FINAL on the same pipe. The next message selection excludes it.

An actual remote-weight change re-evaluates a pending `zlink_send_async()` operation for its exact
pipe. If the weight becomes `0` before the message begins, the completion is `ZLINK_SEND_TERMINAL`
with `terminal_errno == ECONNREFUSED`; a change from `0` to a positive value permits retry without
another write-activation event.

An active duplicate keeps its own latest value while standby and uses it if that same pipe is
selected later. Setting the Application maximum below 10 bytes does not prevent pair readiness,
FLOWSTATE, or WEIGHT delivery; malformed CONTROL behavior remains owned by ZMP.

## 6. Directed raw send

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid(
  void *s_,
  const zlink_routing_id_t *target_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

This sends one ordinary raw multipart part to the peer identified by `target_rid_`. Every part must
use the same target. `flags_` is `ZLINK_SEND_FLAGS_NONE` or `ZLINK_SEND_FLAGS_DONTWAIT`. This API
creates no request sequence or completion handler.

When a binding prepares routed asynchronous admission, it obtains the exact application-pipe
identity of the current RID route through the following APIs.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_select_routed_submit_target(
  void *socket_,
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_part_transport_pair(
  void *s_,
  const zlink_routing_id_t *target_rid_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

A ROUTER requires the target RID in `router_rid_or_null_`. Selection is a value snapshot that
reserves neither the pipe nor credit. Exact submit uses only the current application pipe whose
RID, pair ID, and generation match. HWM returns `ZLINK_SUBMIT_BACKPRESSURED`; detach or a stale
generation returns `ZLINK_SUBMIT_NOT_CONNECTED`. It does not reselect another pipe for the same
RID. Once the first multipart part is accepted, a fence that sends the remaining parts only to that
exact pipe remains through FINAL, and failure rolls back the complete record. [Internal
structure](#12-internal-structure) explains how the binding serializes this multipart attempt.

## 7. Raw request submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_router_request_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_);

ZLINK_EXPORT zlink_submit_result_t zlink_router_request_transport_pair_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_);
```

This submits an asynchronous request payload to `peer_rid_` one part at a time. An intermediate
part uses `ZLINK_PART_MORE`, `timeout_ms_ == 0`, `handler_ == NULL`, and `userdata_ == NULL`. The
last part uses `ZLINK_PART_FINAL` and a non-null `handler_`. If the last call uses
`timeout_ms_ == 0`, it uses the `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` default.

If the final submit returns `ZLINK_SUBMIT_OK`, exactly one completion is delivered to `handler_`.
A failed submit does not invoke the handler. Ownership of the callback's `parts_` and each message
moves to the callback, which releases them exactly once. For a valid error reply, the callback
receives the parts after the first 4-byte Big Endian errno part and a non-OK
`zlink_request_result_t` mapped from that errno. If the first error-reply part is absent, is not
4 bytes, or contains `0`, the callback receives `ZLINK_REQUEST_PROTOCOL_ERROR` and a part count of
`0`.

The final request submit must also pass the selected pair's
[pending-request admission limit](../systems/06-auto-hwm.en.md#pending-request-admission). If
capacity is unavailable, it immediately returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN`
regardless of send flags or `SNDTIMEO`, publishes no part of that request on the wire, and does not
invoke the handler. This reason remains distinct from physical-queue HWM and does not change
ordinary-send admission on the same pipe.

```mermaid
sequenceDiagram
    participant App as Application
    participant R as ROUTER (Core)
    participant P as Peer
    App->>R: Submit intermediate part (ZLINK_PART_MORE, no handler)
    App->>R: Submit final part (ZLINK_PART_FINAL, handler_)
    Note over R: Register pending correlation before<br/>the first payload header is visible on the wire
    R->>P: Deliver request record
    P-->>R: Reply or terminal failure
    R-->>App: Invoke handler_ (exactly once)
    Note over App: Ownership of parts_ moves to the callback;<br/>the callback releases them exactly once
```

The diagram shows the normal path after a successful final submit. A failed submit does not invoke
the handler and follows the discard rules in [section 3](#3-part-sequences-and-ownership).

An exact-target request uses `zlink_router_request_transport_pair_part()`. RID and pair-identity
validation, no rerouting, the multipart fence, and rollback match exact raw submit. Core registers
pending correlation before the request kind and sequence in the first payload's ZMP header become
visible on the wire. On submit failure, it removes that pending entry and the completion reservation
and does not invoke the handler. The binding serializes the attempt in the same way as raw send, as
described in [Internal structure](#12-internal-structure).

## 8. Raw request and message receive

```c
ZLINK_EXPORT zlink_recv_result_t zlink_router_recv_part(
  void *router_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);

ZLINK_EXPORT zlink_recv_result_t zlink_router_recv_part_v2(
  void *router_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_,
  uint64_t *transport_pair_id_out_,
  uint64_t *transport_pair_generation_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

This returns one part from a complete raw record. Every output pointer is required. `flags_` is
`ZLINK_RECV_FLAGS_NONE` or `ZLINK_RECV_FLAGS_DONTWAIT`. A non-blocking call with no record available
returns `ZLINK_RECV_NO_DATA` and `EAGAIN`.

When `has_more_out_ == ZLINK_PART_MORE`, the next call must receive the next part of the same
record. `zlink_router_recv_part_v2()` has the same receive and ownership contract and additionally
returns the source transport pair ID and generation. Every part of the same record returns the same
RID, pair ID, and generation. The caller uses this snapshot for a reply or an exact-target follow-up
operation without reselecting another pair. `ZLINK_PART_FINAL` completes the record's receive
sequence. Use the output combinations in [section 2](#2-raw-receive-record-classification) to
determine whether a reply is required.

The returned payload has no internal request metadata. The routing ID, pair identity, and opaque
reply token obtained from the first part of a multipart request are repeated for every remaining part
of the same record, but are not retained in the message itself.

## 9. Raw reply submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_router_reply_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  uint64_t request_seq_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

This sends a reply part for a request returned by `zlink_router_recv_part()`. `peer_rid_` and the
nonzero `request_seq_` use the RID and opaque reply token returned by that receive record without
modification. A multipart reply uses the same two values for every call. A successful
`ZLINK_PART_FINAL` completes the reply.

Core finds the target by `(peer_rid_, request_seq_)` and writes the target's original wire sequence
and reply kind into the first reply payload's ZMP header. Because the public API adds no sequence
payload part, the peer's completion callback receives only the parts supplied by the application.

Raw replies and error replies are submitted exactly once on the [completion progress
lane](../glossary.en.md#completion-progress-lane), a separate path that handles only terminal-reply
progress and bypasses HWM admission. This lane is not subject to application byte HWM, manual HWM,
LWM, or Core budget reservation. This function therefore does not return
`ZLINK_SUBMIT_BACKPRESSURED` because of that capacity and enters no readiness-wait or retry path.
Connection, lifecycle, argument, state, and allocation failures
terminate immediately with the corresponding `zlink_submit_result_t` at the call.

When a reply has no target route, the result is `ZLINK_SUBMIT_NOT_CONNECTED` with `errno ==
ENOTCONN`. The same rule applies both when no completion pipe is found for the target and when an
already selected target disappears while the reply is being committed. This failure is not
backpressure, so `ZLINK_POLLOUT` does not make a retry of this one-shot reply viable. In both cases
the parts already handed in are consumed and are not returned to the caller.

The completion progress lane processes valid receive-flow control before application kinds. A
reply or error reply completes the one pending request identified by its sequence. If data or a
request arrives on this lane, Core does not invoke a callback with that frame; it terminates the
pair with `EPROTO`, and each existing pending request completes once with the disconnect result.

## 10. Results and readiness

Submit APIs return `zlink_submit_result_t`, receive APIs return `zlink_recv_result_t`, and option
APIs return `zlink_config_result_t`. The [errno map](../03-errors.en.md#result-and-errno-mapping)
defines the mapping between each result and `zlink_errno()`.

ROUTER `ZLINK_POLLIN` means that a complete raw record can be received. For ordinary sends and
requests, `ZLINK_POLLOUT` indicates that retrying a submit after
[backpressure](../glossary.en.md#backpressure), the state in which additional submissions are
limited because the receiver cannot keep up, is worthwhile. It does not guarantee that the next
submit succeeds. An application that needs a definitive answer for each operation uses
`zlink_send_async` and its completion notification. This readiness contract does not apply to raw
replies.

## 11. Receive flow state

A ROUTER paired with [DEALER](06-dealer.en.md) peers over a completion lane can ask those peers to
stop and resume sending to it. `zlink_socket_set_receive_flow_state()` stores one socket-wide state.
[Socket Common](README.en.md) owns the function declaration, and [Errors](../03-errors.en.md) owns
the result table.

The state belongs to the socket, not to a routing ID. There is no per-peer flow-state call. One call
sends the state over the completion lane of every ready transport pair of this ROUTER, so every peer
receives the same state. A peer that becomes ready later also receives the socket's current state
over its new completion lane. A routing ID selects the destination of a send; it does not select a
receive-flow state.

The state is an absolute value, not a counter. Setting the current state again succeeds and sends
nothing.

Each state change includes a flow epoch that increases within one connection generation, and each
frame contains the pair ID and generation on which it was written. A frame is applied only when it
names the receiving pipe's current pair and generation and its epoch is greater than the last epoch
accepted for that generation. A frame from a different pair ID, a frame with a zero pair ID or
generation, a frame for a pair absent from the transport-pair table, or a frame from a pipe other
than the registered completion pipe is consumed without an event. A frame is not applied and is
reported as `ZLINK_EVENT_FLOW_STATE_STALE` only when it names the current pair ID but has a
mismatched generation, or when its epoch is duplicate or regressive within the same generation. A
routing ID remains stable across a reconnect, but a pair generation does not. State published by a
peer before a reconnect is therefore never applied to the replacement connection. A new generation
starts from the state that the socket sends when the pair becomes ready.

A remote PAUSE blocks only sends to the paused peer and does not affect routes to other peers. It is
an independent blocker composed with byte HWM, transport waits, and termination, so clearing it
does not by itself admit the next send. Send results and readiness remain unchanged. A blocked
non-blocking send continues to report `ZLINK_SUBMIT_BACKPRESSURED` with `errno == EAGAIN`, and
mandatory routing retains the behavior defined in [section 6](#6-directed-raw-send).

A remote PAUSE applies from the next message boundary and does not split a multipart record. If
this ROUTER has already accepted a record's routing-ID part, it sends the remaining parts before
the pause takes effect.

The [Monitoring](../06-monitoring.en.md) status snapshot reports the current number of paused peers
together with the applied-transition count, stale count, and pause duration for the whole socket.

## 12. Internal structure

> **Contract ownership for this section** — [Section 6](#6-directed-raw-send) and [section
> 7](#7-raw-request-submit) own the public contracts for directed and exact submit and request
> submit. This section explains how a binding serializes multipart attempts on top of those
> contracts.

Because each part call has a separate public API scope, a binding holds a socket-local attempt gate
only for one `DONTWAIT` attempt from the first part through FINAL. On failure, it releases the gate
immediately after Core rolls back the sequence and does not hold it while waiting for
`BACKPRESSURED` readiness. Request submit also makes one attempt from the first request part through
FINAL under the same short socket-local attempt gate as raw send. This gate is neither a new Core
multipart API nor a public FIFO contract.

## 13. Implementation and contract test verification requirements

The following behaviors are verified using only the public surface: ROUTER send, request, receive,
and reply functions; ROUTER option set and get; return values and errno; the
`zlink_reply_handler_fn` completion; and event and status snapshots. Each item maps to one test.

**Options**
- When `zlink_get_router_option()` is called, `*optvallen_` is the input capacity, and on success it is updated to the number of bytes written.
- Each option's default value is returned: `MANDATORY` `1`, `PROBE` `0`, `REQUEST_TIMEOUT_MS` `5000`, and `WEIGHT` `100`.
- When `ZLINK_ROUTER_OPT_MANDATORY` is positive, a directed submit to a routing ID without a connected pipe fails with `ZLINK_SUBMIT_NOT_CONNECTED`, and the getter returns `0` or `1`.
- When `ZLINK_ROUTER_OPT_PROBE` is positive, an empty raw message is sent when a connection is established so that the peer can observe the connection and routing ID, and the getter returns `0` or `1`.
- Setting `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID` before connect identifies the pipe created by the next `zlink_connect()` by that local alias.

**Peer-weight delivery**

- If different weights are configured on a ROUTER and its peer DEALER before bind or connect, each
  scheduler uses the peer's exact value after the pair becomes ready, and a peer with value `0` is
  excluded from outbound candidates.
- Dynamically changing both weights after a network or inproc pair is ready produces
  `PEER_WEIGHT_CHANGED` with the new weight in `value`; the event transport lane, pair ID, and
  generation match the paired Application pipe to which the value was applied.
- Setting or synchronizing weight adds no application record to public receive or the Completion
  lane, and setting the same value again produces no duplicate monitor event.
- Changing weight more than once while an Application multipart is open preserves the peer-visible
  multipart as one atomic record, and only the latest value is reflected after FINAL or rollback.
- If a pipe's remote weight becomes `0` after the first part of an Application multipart is
  accepted, the same pipe carries every remaining part through FINAL and is excluded starting with
  the next message selection.
- A remote-weight change re-evaluates a pending `zlink_send_async()` operation for its exact pipe:
  if the weight becomes `0` before the message begins, the completion is `ZLINK_SEND_TERMINAL` with
  `terminal_errno == ECONNREFUSED`; a change from `0` to a positive value permits retry without
  another write-activation event.
- Setting an Application maximum below 10 bytes does not prevent pair readiness, FLOWSTATE, or
  weight changes observed through peer selection and monitoring.
- After reconnect, peer selection and monitoring reflect the current weight on the new generation.
  Promoting an active standby uses the value that standby most recently received.

**Record classification and receive**
- An ordinary raw record returns `request_seq_out_ == 0`; a received request returns a nonzero opaque reply token. The token may normally equal the wire sequence, but that equality is not a contract.
- Every part of one multipart record returns the same routing ID and opaque reply token, and `zlink_router_recv_part_v2()` additionally returns the same pair ID and generation for every part of that record.
- Replies and terminal failures for a request started with `zlink_router_request_part()` are not returned as receive records and are delivered through `zlink_reply_handler_fn`.
- A non-blocking receive with no record available returns `ZLINK_RECV_NO_DATA` and `EAGAIN`.
- On successful receive, ownership of the part moves to the caller, which releases it with exactly one `zlink_msg_close()`; on failure, ownership does not move.
- When `has_more_out_ == ZLINK_PART_MORE`, the next call returns the next part of the same record; `ZLINK_PART_FINAL` completes the record's receive sequence.
- A reply or error reply received through `zlink_router_recv_part()` or `_v2()` returns no payload and terminates the pair with `EPROTO`.
- Raw-sending a data or request payload returned by `zlink_router_recv_part()` or `_v2()` does not restore request-reply semantics.
- Passing a ROUTER to the common `zlink_recv_part()` surface is rejected as unsupported.
- When different physical sources with the same RID send requests carrying the same live wire sequence, ROUTER returns distinct opaque reply tokens. Reverse or out-of-order replies use each token's saved exact source pipe, pair, and original wire sequence, so they complete only their original request.
- If the same physical source reuses a live wire sequence, ROUTER terminates that pair with `EPROTO` and does not deliver the duplicate request to application receive.

**Part sequences**
- A send API consumes the content of `part_` on both success and failure and leaves it as an initialized zero-length message; the same `part_` cannot be resubmitted after failure.
- If an intermediate or final submit in an open sequence fails, no part of that record becomes visible to the peer, and the next submit starts the first part of a new record.
- A failed request submit creates no request sequence and invokes no handler.
- After a reply-sequence failure, the reply-token and peer-RID pair remains valid until a successful `ZLINK_PART_FINAL` or the end of the request lifecycle, and a retained complete reply can be resubmitted from its first part.
- A non-empty group on the first request or reply part yields `ZLINK_SUBMIT_INVALID_ARGUMENT` and `EINVAL`; the input part is consumed and the peer receives no part of that record. A failed request does not invoke its handler, and a failed reply can be submitted again with group-free payload using the same source routing ID and opaque reply token.

**Directed and exact submit**
- `zlink_send_part_rid()` creates no request sequence or completion handler.
- Exact submit uses only the current application pipe whose RID, pair ID, and generation match: HWM returns `ZLINK_SUBMIT_BACKPRESSURED`, detach or a stale generation returns `ZLINK_SUBMIT_NOT_CONNECTED`, and another pipe for the same RID is not reselected.
- A failure after the first multipart part is accepted rolls back the complete record, so no partial record becomes visible to the peer.

**Request completion**
- If the final submit returns `ZLINK_SUBMIT_OK`, exactly one completion is delivered to `handler_`; a failed submit does not invoke the handler.
- If the last call uses `timeout_ms_ == 0`, it uses the `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` default.
- Ownership of the callback's `parts_` and each message moves to the callback, which releases them exactly once.
- A valid error reply delivers a non-OK `zlink_request_result_t` mapped from its errno and the payload after the errno part to the Core C callback. An absent errno part, a part whose size is not 4 bytes, or a zero value completes with `ZLINK_REQUEST_PROTOCOL_ERROR` and a part count of `0`.
- If an exact request submit fails, the handler is not invoked, and no subsequent completion for that request is delivered.
- When a pair reaches its pending-request admission limit, the final submit immediately returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` regardless of send flags or `SNDTIMEO`, without wire publication. Reply or timeout returns capacity and wakes a retry; another exact pair and ordinary send remain unaffected.

**Replies and the completion lane**
- `zlink_router_reply_part()` uses the `peer_rid_` and opaque `request_seq_` token returned by the receive record without modification. Core re-stamps the original wire sequence onto that token's exact source pipe and pair, and a successful `ZLINK_PART_FINAL` completes the reply.
- Raw replies and error replies do not return `ZLINK_SUBMIT_BACKPRESSURED` because of completion-lane capacity; connection, lifecycle, argument, state, and allocation failures terminate immediately with the corresponding `zlink_submit_result_t` at the call. When there is no target route (no completion pipe found, or the target disappears mid-commit), the call returns `ZLINK_SUBMIT_NOT_CONNECTED` with `errno == ENOTCONN`; this is not backpressure, so `ZLINK_POLLOUT` does not make a retry viable.
- A reply or error reply on the completion progress lane invokes the matching public request completion once; data or a request terminates the pair with `EPROTO` without being delivered as callback payload.

**Readiness**
- `ZLINK_POLLIN` is set when a complete raw record can be received; `ZLINK_POLLOUT` indicates only that a retry after backpressure is worthwhile and does not guarantee that the next submit succeeds.
- The readiness contract does not apply to raw replies.

**Receive flow state**
- Setting the current state again succeeds and sends nothing.
- One call gives every ready transport pair the same state, and a peer that becomes ready later also receives the socket's current state.
- A frame from a different pair ID, a frame with a zero pair ID or generation, a frame for an unregistered transport pair, or a frame from a pipe other than the registered completion pipe is consumed without an event. `ZLINK_EVENT_FLOW_STATE_STALE` occurs only when a frame names the current pair ID but has a mismatched generation, or when its epoch is duplicate or regressive within the same generation.
- State published by a peer before a reconnect is not applied to the replacement connection.
- A remote PAUSE blocks only sends to the paused peer: routes to other peers are unaffected, a blocked non-blocking send reports `ZLINK_SUBMIT_BACKPRESSURED` with `errno == EAGAIN`, and clearing PAUSE alone does not admit the next send.
- A remote PAUSE applies from the next message boundary: a record whose routing-ID part has already been accepted sends its remaining parts before the pause takes effect.
- The [Monitoring](../06-monitoring.en.md) status snapshot provides the current number of paused peers, the socket-wide applied-transition count, stale count, and pause duration.

<!-- zlink-nav:start -->
[Socket Index](README.en.md) | [Previous: DEALER](06-dealer.en.md) | [Next: STREAM](08-stream.en.md)
<!-- zlink-nav:end -->
