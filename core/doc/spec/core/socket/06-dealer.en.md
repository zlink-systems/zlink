---
title: "Socket — DEALER"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/socket/06-dealer/) | English

<!-- zlink-nav:start -->
[Socket Index](README.en.md) | [Previous: XSUB](05-xsub.en.md) | [Next: ROUTER](07-router.en.md)
<!-- zlink-nav:end -->

# Socket — DEALER

> **What this chapter defines** — the public contract for DEALER socket request routing and
> [result/errno](../03-errors.en.md#result-and-errno-mapping).

## 1. DEALER overview

DEALER is an asynchronous raw [socket](../glossary.en.md#socket) that receives from multiple peers
in fair-queue order and sends to connected peers using round-robin or weight-based selection. The
application pull-receives ordinary DATA and can submit requests to a ROUTER logical route selected
by Core.

This document defines the public contract for DEALER-specific options, outbound peer selection,
part sequences and ownership, request submission and pull completion, and receive flow state. Its
audience is developers who map this contract to the C API and each
language binding, and application developers who use DEALER.

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| Socket creation, common options (HWM, reconnect, timeout), and the declaration of `zlink_socket_set_receive_flow_state` | [Socket Common](README.en.md) |
| Request-reply kinds, sequences, and ZMP header byte layout and validation | [ZMP](../protocol/01-zmp.en.md) |
| Mapping between each result and `zlink_errno()` | [Errors](../03-errors.en.md#result-and-errno-mapping) |
| Auto HWM budget calculation and admission | [Auto HWM](../systems/06-auto-hwm.en.md) |
| Flow statistics in the status snapshot | [Monitoring](../06-monitoring.en.md) |
| Contract for the paired ROUTER socket | [ROUTER](07-router.en.md) |

## 2. DATA receive and request completion

DEALER receives only ordinary DATA through `zlink_recv_part()`. It is not a responder socket that
receives or replies to inbound typed REQUEST records. Replies, timeouts, and terminal results for a
REQUEST submitted by DEALER do not appear on ordinary receive; they are returned as REQUEST records
from `zlink_completion_recv()`.

On a DEALER-ROUTER single connection, DATA, REPLY, and error reply sent by the ROUTER use the same
inbound physical FIFO. When the physical head is DATA, public DATA receive consumes the record; when
it is REPLY or error reply, the socket-local completion queue consumes the record. REPLY does not
appear in `zlink_recv_part()`, and DATA does not appear in `zlink_completion_recv()`.

On a DEALER-ROUTER single connection, DATA sent first by the ROUTER and a later REPLY or error reply
use the same FIFO. If DEALER does not dequeue the preceding DATA or keeps local PAUSED in effect, the
REPLY cannot overtake it and the request timeout can create the terminal completion first.

```mermaid
sequenceDiagram
    participant App as DEALER application
    participant D as DEALER Core
    participant R as ROUTER Core
    App->>D: zlink_request_part(FINAL, context)
    D->>D: Reserve completion ID and slot
    D->>R: REQUEST
    R-->>D: REPLY or terminal result
    D->>D: Store in REQUEST completion queue
    D-->>App: ZLINK_POLLCOMPLETION readiness
    App->>D: zlink_completion_recv(DONTWAIT)
    D-->>App: Result and reply multipart
```

## 3. Outbound peer selection

The following rules determine which peer receives a round-robin or weighted send. The weight is the
absolute value that each peer advertises through its own `ZLINK_DEALER_OPT_WEIGHT` or
`ZLINK_ROUTER_OPT_WEIGHT`. [§8](#8-dealer-options) defines the DEALER option.

A candidate is a connected outbound peer whose advertised weight is positive. A peer with weight
`0` is excluded from the candidate set. If every known peer has weight `0`, a submit may fail with
`ZLINK_SUBMIT_NOT_ADMITTED`.

Each candidate has an accumulator that starts at `0`. The following selection procedure runs once
for each message sent.

1. Add each candidate's weight to its accumulator.
2. Select the candidate with the largest accumulator. If several candidates have the same
   accumulator, select the one with the smallest identifier.
3. Subtract the sum of all candidate weights from the selected candidate's accumulator.

Equal weights are not a separate rule. Candidates with equal weights follow the same three steps and
are therefore selected in turn. The same procedure applies when there is only one candidate. Adding
and subtracting the same value leaves its accumulator unchanged.

This procedure spreads consecutive selections instead of grouping one candidate's share into a
single run. With weights `100` and `300`, the repeating order is `second, first, second, second`, not
three consecutive sends to the heavier peer followed by one send to the lighter peer. With enough
messages, the selection frequency matches the configured ratio.

For an ordinary `zlink_send_part()`, the selection procedure applies only to the message that a peer accepts. If the selected
candidate has no write capacity, it is excluded for that attempt and the procedure applies to the
peer that accepts the message instead. This fallback does not change the configured weight, and the
peer returns to the candidate set when it reports write capacity again. A message rejected for
exceeding a size limit is not retried against another candidate, because every candidate would
reject it for the same reason.

When a `NONE FINAL` waits for admission, FINAL fixes one configured endpoint. Ordinary DATA selects
from compatible positive-weight logical routes; typed requests select from positive-weight logical
routes confirmed as ROUTER during handshake. The operation does not change to another endpoint
while waiting for HWM or a temporary disconnect.

A `DONTWAIT FINAL` ordinary send or request pins no endpoint. If no candidate has write capacity in
its single admission attempt, it returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` and a nonzero
wait token whose target is the whole candidate peer set. When any candidate reports write capacity
or a new peer connects, Core publishes one `ZLINK_COMPLETION_WRITABLE` record, and the resubmission
selects a peer again with the selection procedure at that time. A DEALER with `0` peers right after
connect still receives a wait token.

The identifier in step 2 is the peer routing ID, compared as a byte string. An absent routing ID is
an empty byte string, so it sorts before every non-empty identifier. Peers with the same identifier,
including peers that all have no routing ID, are ordered by the endpoint through which the
connection was established; if the endpoint is also the same, they are ordered by local connection
attachment order. A reconnect creates a new connection whose accumulator starts at `0`. Its
identifier is unchanged, so its sorting position is the same as before.

Two processes configured with the same peers and weights produce the same selection order. An
application may depend on this order when candidate identifiers are distinct. When ordering falls
through to local connection attachment order, it is deterministic within one process but is not
reproducible across processes.

When the candidate set changes, the remaining candidates retain their accumulators, preserving the
configured ratio. A new connection starts at `0`, and a disconnected peer discards its accumulator
with the connection. A peer excluded only temporarily because of [backpressure](../glossary.en.md#backpressure)
or weight `0` retains its accumulator and continues from that value when it becomes a candidate
again.

Peer selection is fixed for one Application message. If the selected pipe's remote weight changes
to `0` after the first part of a multipart has been accepted, all remaining parts through
`ZLINK_PART_FINAL` use that same pipe. Weight `0` excludes the pipe only when the next message is
selected.

## 4. Part sequences and ownership

`*_part` send calls form one multipart sequence from `ZLINK_PART_MORE` through `ZLINK_PART_FINAL`.
While a sequence is open, another send helper family cannot be interleaved on the same handle.

When a valid initialized `part_` is passed to a send API, the function consumes its message content
on both success and failure and leaves it as an initialized zero-length message. Regardless of the
call result, the caller therefore cannot read the pre-submit payload again or resend the same
content. Payload that may need to be resent must be retained in a separate message before the call.

Each send helper family stages successful intermediate parts as one record until
`ZLINK_PART_FINAL` succeeds. If an intermediate or final submit in an open sequence fails, Core
atomically discards the previously staged parts and the failed part, then closes the sequence. No
part of the record becomes visible to the peer. The failed call also consumes `part_`, and the next
submit starts the first part of a new record. A failed request submit returns completion ID `0` and
creates neither a completion nor a context echo.

The `part_out_` passed to a receive API must be an initialized `zlink_msg_t` before the call. On
success, ownership of the received part moves to the caller, which releases it exactly once with
`zlink_msg_close()`. On failure, ownership of a received part does not move.

<a id="8-results-and-readiness"></a>

## 5. Results and readiness

Submit APIs return `zlink_submit_result_t`, receive APIs return `zlink_recv_result_t`, and option
APIs return `zlink_config_result_t`. The [errno map](../03-errors.en.md#result-and-errno-mapping)
defines the mapping between each result and `zlink_errno()`.

DEALER `ZLINK_POLLIN` means that a DATA record can be received. For ordinary sends and
requests, `ZLINK_POLLOUT` indicates that retrying a submit after backpressure is worthwhile, but it
does not guarantee that the next submit will succeed. While an unread `ZLINK_COMPLETION_WRITABLE`
record exists, `ZLINK_POLLOUT` and `ZLINK_POLLCOMPLETION` are level-held, and the precise per-target
signal is that record's token and context. Results for operations retained by Core are received
through `ZLINK_POLLCOMPLETION` and `zlink_completion_recv()`. Request replies do not appear under
`ZLINK_POLLIN`.

## 6. Receive flow state

A DEALER connected to a DEALER or ROUTER peer can ask peers that send to it to stop and resume
transmission. `zlink_socket_set_receive_flow_state()` stores one socket-wide state. Because a DEALER's
transport pair has count `1`, Core sends that state, for every ready peer, over the control path of
that peer's Application connection. [Socket Common](README.en.md)
owns the function declaration, and [Errors](../03-errors.en.md) owns the result table.

This state is an absolute value, not a counter. Setting `ZLINK_RECEIVE_FLOW_PAUSED` twice represents
one pause; the second call succeeds without sending anything. There is no nesting count and no rule
requiring a matching number of resumes. The socket stores exactly one state, so it cannot pause one
peer while leaving another peer running.

A flow-state frame carries a flow epoch scoped to the connection on which the frame was written.
There is no public pair-ID or generation field and no `Zlink-Pair-Id` or
`Zlink-Pair-Generation` wire property. Using internal connection identity, Core applies the frame
only to the connection on which it was written. A frame whose identity does not match, including a
frame from a replaced connection, is consumed internally without a public event and increments only
the `flow_state_stale_total` counter. A duplicate or regressing epoch on the same connection is not
applied and is reported as `ZLINK_EVENT_FLOW_STATE_STALE` with
`ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH`. A frame from a previous connection is therefore
never applied to the connection that replaced it.

When a connection becomes ready, Core sends the socket's current state over the new Application connection. A
peer that connects or reconnects while this socket is paused learns the pause without another call.
A socket that has never set a state sends nothing because a new connection already assumes RUNNING.

A remote PAUSE blocks transmission to that peer. It is an independent blocker that composes with
existing blockers. Byte [HWM](../glossary.en.md#hwm), which caps bytes retained in a queue,
transport wait, and termination each continue to block transmission; a send is accepted only when
none of them applies. Clearing remote pause therefore does not by itself make the next send succeed.
Send results and readiness are unchanged. A blocked non-blocking send still reports
`ZLINK_SUBMIT_BACKPRESSURED` with `errno == EAGAIN`, and `ZLINK_POLLOUT` retains the meaning defined
in [§5 Results and readiness](#5-results-and-readiness). A remote RESUME is one of the wake edges
that publishes a `ZLINK_COMPLETION_WRITABLE` record for the wait token that send received.

A remote PAUSE takes effect at the next message boundary and does not split a message. A message
whose first byte has already reached the pipe, and a message whose first part the socket has already
accepted, sends all remaining parts before the pause applies. The pause applies from the following
message.

The [Monitoring](../06-monitoring.en.md) status snapshot provides the number of peers this socket
currently sees as paused, the numbers of applied pause and resume transitions, the number of frames
rejected as stale, and the duration of the most recently completed pause.

## 7. Public types

The enum numbers in this section and [§8 DEALER options](#8-dealer-options) are public ABI values.

```c
typedef enum zlink_part_flag_t {
  ZLINK_PART_FINAL = 0,  // Current part is the last part of the record
  ZLINK_PART_MORE  = 1   // Another part follows in the same multipart record
} zlink_part_flag_t;

```

The receive API's `has_more_out_` uses the same two `zlink_part_flag_t` values.

<a id="2-dealer-options"></a>

## 8. DEALER options

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_dealer_option(
  void *handle_,
  zlink_dealer_option_t option_,
  const void *optval_,
  size_t optvallen_);

ZLINK_EXPORT zlink_config_result_t zlink_get_dealer_option(
  void *handle_,
  zlink_dealer_option_t option_,
  void *optval_,
  size_t *optvallen_);

typedef enum zlink_dealer_option_t {
  ZLINK_DEALER_OPT_PROBE              = 0x3201,  // int, 0=off, positive=on, getter returns 0/1 (default 0). Sends an empty raw message when a connection is established so the peer can observe the connection and routing ID
  ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS = 0x3202,  // Nonnegative int, milliseconds (default 5000). Default timeout used by the request API when timeout_ms_ == 0
  ZLINK_DEALER_OPT_WEIGHT             = 0x3203   // int, 0..10000 (default 100). This DEALER's weight advertised to connected peers
} zlink_dealer_option_t;
```

When calling `zlink_get_dealer_option()`, `*optvallen_` is the input capacity of `optval_`. On
success, it is updated to the number of bytes actually written. HWM, reconnect, and timeout options
that are not DEALER-specific use `zlink_set_option()` and `zlink_get_option()`.

A weight outside `0..10000` is rejected and is not clamped. Values in `0..100` retain the meaning
they had before the range was widened.

The common [`ZLINK_OPT_CONFLATE` contract](README.en.md#conflation) does not allow DEALER to enable
frame-level conflation. Setting `1` returns `ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP`; setting `0`
succeeds as a no-op, and the getter returns `0`.

`ZLINK_DEALER_OPT_WEIGHT` is the absolute value that a peer uses when selecting this DEALER as an
outbound candidate. DEALER and ROUTER advertise their own values independently, so each direction
uses the value advertised by the other socket.

The public weight result follows this order.

1. A value set before bind or connect applies after the single Application pipe becomes ready.
2. A dynamic change applies the new absolute value, including `0`, to the peer scheduler.
3. An actual change emits `PEER_WEIGHT_CHANGED` with the value and the Application pipe's lane and
   connection ID. Repeating the same value emits no additional event.
4. Reconnect applies the current configured value to the new connection.

The network wire, inproc delivery, CONTROL size boundary, multipart deferral, and the lifetime and
stale-delivery ownership of that selected pipe are defined by the
[ZMP request-reply lane](../protocol/01-zmp.en.md#41-request-reply-lane),
[decode](../protocol/01-zmp.en.md#7-decode-validation), and
[peer-weight owner](../protocol/01-zmp.en.md#peer-weight-control) contracts. Neither transport path
creates a weight record on public receive or the socket-local completion queue.

If the applied value becomes `0` after a multipart has selected a pipe, that message completes
through FINAL on the same pipe. The next message selection excludes it.

An actual remote-weight change re-evaluates a DONTWAIT send or request that holds a wait token. A
wait token does not end when the weight becomes `0`. A change from `0` to a positive value publishes
a `ZLINK_COMPLETION_WRITABLE` record for the SEND or REQUEST wait token.

An active duplicate keeps its own latest value while standby and uses it if that same pipe is
selected later. Setting the Application maximum below 10 bytes does not prevent pair readiness,
FLOWSTATE, or WEIGHT delivery; malformed CONTROL behavior remains owned by ZMP.

## 9. Functions

### zlink_send_part

Sends ordinary DATA.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part(
  void *s_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);
```

Part consumption and record discard on failure follow
[§4 Part sequences and ownership](#4-part-sequences-and-ownership).
A `DONTWAIT FINAL` makes exactly one admission attempt. If it is admitted immediately, it has ID
`0` and no completion. If no candidate peer has write capacity (HWM, byte credit, remote PAUSE,
weight `0`, and `0` peers included), it returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` and a
nonzero wait token, and Core does not retain the payload. When any candidate peer gains write
capacity, Core produces exactly one `ZLINK_COMPLETION_WRITABLE` record for that token
(`ZLINK_SEND_ADMITTED`, the same `user_context`, empty `peer_rid`), and the caller resubmits its
retained record with `DONTWAIT`. The token ends with the WRITABLE record; socket close or context termination
ends it internally and delivers no record. `NONE FINAL` snapshots
`SNDTIMEO` on entry, waits through admission, and finishes with ID `0`. [Socket Common](README.en.md#part-send-and-pending-admission)
owns the detailed result, errno, and context contract.

---

### zlink_request_part

Submits a request payload one part at a time to a ROUTER logical route selected by Core.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_request_part(
  void *s_,
  const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);
```

DEALER requires `target_router_rid_or_null_ == NULL`. A `MORE` call requires
`timeout_ms_ == 0` and `user_context_ == NULL`. An admitted `FINAL` returns a nonzero REQUEST ID
and produces exactly one REQUEST completion: reply, timeout, or terminal. `timeout_ms_ == 0`
snapshots the `ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS` value, whose default is 5,000 ms. On FINAL,
`user_context_` may be `NULL` or an opaque pointer for both `NONE` and `DONTWAIT`; a successful
completion returns it unchanged.

Candidates are positive-weight logical routes confirmed as ROUTER during handshake. A DEALER peer
remains a DATA candidate but is excluded from request candidates. No known ROUTER returns
`ZLINK_SUBMIT_NOT_CONNECTED` with `ENOTCONN`; known ROUTERs all at weight `0` return
`ZLINK_SUBMIT_NOT_ADMITTED` with `ECONNREFUSED`. `NONE FINAL` waits within `SNDTIMEO` for an
unknown endpoint to complete handshake and an eligible ROUTER to appear, then applies this
decision. Only a `NONE FINAL` that selects a detached known positive-weight ROUTER waits on that
configured endpoint, and the endpoint selected at FINAL does not change before the operation
terminates.

`DONTWAIT FINAL` makes one admission attempt and pins no endpoint. If no ROUTER is eligible (no
known ROUTER, all at weight `0`, or `0` peers right after connect) or the selected ROUTER has no
write capacity, it returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` and a nonzero wait token
whose target is the whole request candidate set. Core retains no request payload. When any
candidate reports write capacity, a peer confirmed as ROUTER connects, or a weight changes from
`0` to a positive value, Core publishes one `ZLINK_COMPLETION_WRITABLE` record, and when the caller
resubmits the same request, the selection procedure at that time picks a ROUTER again.

The reply timeout starts at local send-queue admission, that is, when `ZLINK_SUBMIT_OK` is
returned. It does not start while a wait token is outstanding. When the submit-time transport
pair terminates after admission, the request ends at once with `ZLINK_REQUEST_NOT_CONNECTED`,
whatever the cause, per the [Socket Common §6 completion table](README.en.md#request-and-reply), and
the payload is not replayed. [Socket Common](README.en.md#completion-pull-and-ownership) owns completion ownership and
close.

---

### zlink_recv_part

Returns one part from a DATA record.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part(
  void *s_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

`part_out_` and `has_more_out_` are required; `source_rid_out_` is optional. On a successful receive,
the source is `NULL`. The `NONE` `RCVTIMEO`, `DONTWAIT`, output ownership and invariance,
multipart owner, and flag-error rules follow [Socket Common](README.en.md#zlink_recv_part). Request
replies do not appear through this function.

## 10. Implementation and contract-test verification requirements

Verify the following using only the public surface: DEALER option set/get, `zlink_send_part`,
`zlink_request_part`, `zlink_recv_part`, `zlink_completion_recv`, return values and errno, and the
[Monitoring](../06-monitoring.en.md) status snapshot. Each item maps to one test.

**Options**

- A `ZLINK_DEALER_OPT_WEIGHT` value outside `0..10000` is rejected and is not clamped.
- When `zlink_get_dealer_option()` succeeds, `*optvallen_` is updated to the number of bytes actually written.
- When `ZLINK_DEALER_OPT_PROBE` is set to a positive value, the peer can observe the connection and routing ID through an empty raw message when the connection is established, and the getter returns `0` or `1`.
- When the final request part is submitted with `timeout_ms_ == 0`, the `ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS` value (default `5000`) is used as the timeout.
- On DEALER, setting `ZLINK_OPT_CONFLATE` to `1` returns `ZLINK_CONFIG_NOT_SUPPORTED` with
  `ENOTSUP`; setting `0` succeeds, and the getter returns `0`.

**Peer-weight delivery**

- If different weights are configured on a DEALER and its peer ROUTER before bind or connect, each
  scheduler uses the peer's exact value after the pair becomes ready, and a peer with value `0` is
  excluded from outbound candidates.
- Dynamically changing both weights after a network or inproc pair is ready produces
  `PEER_WEIGHT_CHANGED` with the new weight in `value`; the event transport lane and
  `connection_id` match the Application pipe to which the value was applied.
- Setting or synchronizing weight adds no record to public receive or the socket-local completion
  queue, and setting the same value again produces no duplicate monitor event.
- Changing weight more than once while an Application multipart is open preserves the peer-visible
  multipart as one atomic record, and only the latest value is reflected after FINAL or rollback.
- If a pipe's remote weight becomes `0` after the first part of an Application multipart is
  accepted, the same pipe carries every remaining part through FINAL and is excluded starting with
  the next message selection.
- A remote-weight change re-evaluates a DONTWAIT SEND or REQUEST that holds a wait token. The wait
  token does not end when the weight becomes `0`; a change from `0` to a positive value publishes a
  WRITABLE record for the SEND or REQUEST wait token.
- Setting an Application maximum below 10 bytes does not prevent pair readiness, FLOWSTATE, or
  weight changes observed through peer selection and monitoring.
- After reconnect, peer selection and monitoring reflect the current weight on the new connection.
  Promoting an active standby uses the value that standby most recently received.

**Outbound peer selection**

- Repeated sends to two peers with weights `100` and `300` repeat the selection order `second, first, second, second`.
- Candidates with equal weights are selected in turn, and with enough messages their selection frequencies match the configured ratio.
- If every known peer has weight `0`, a submit may fail with `ZLINK_SUBMIT_NOT_ADMITTED`.
- Two processes configured with the same peers and weights produce the same selection order when their candidate identifiers are distinct.
- A reconnected peer starts again with accumulator `0` and retains its previous sorting position.
- A peer that cannot accept a message because it has no write capacity is excluded only for that message and continues from its retained accumulator when it reports capacity again.
- A DONTWAIT SEND or REQUEST pins no endpoint. If no candidate has write capacity, it returns
  `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` and a nonzero wait token; when any candidate reports
  write capacity or a new peer connects, one WRITABLE record is published, and the resubmission
  selects a peer again. A DEALER with `0` peers still receives a wait token.

**Part sequences and ownership**

- A send API consumes `part_` on both success and failure and leaves it as an initialized zero-length message—the caller cannot read the pre-submit payload again or resend it through the same `part_` after the call.
- If an intermediate or final submit in an open sequence fails, no part of that record is visible to the peer and the next submit starts the first part of a new record.
- A failed request submit returns ID `0` and creates neither a completion nor a context echo.
- On receive success, ownership of the part moves to the caller, which releases it exactly once with `zlink_msg_close()`. On failure, ownership does not move.

**Requests and completion**

- If request FINAL returns `ZLINK_SUBMIT_OK`, it returns a nonzero ID and exactly one REQUEST
  completion for reply, timeout, or terminal. A failed submit returns ID `0` and no completion.
- `NONE` waits within `SNDTIMEO` for an eligible ROUTER; then no known positive-weight ROUTER
  returns `ZLINK_SUBMIT_NOT_CONNECTED` with `ENOTCONN`, and known ROUTERs all at weight `0` return
  `ZLINK_SUBMIT_NOT_ADMITTED` with `ECONNREFUSED`. A DEALER peer is not a typed-request candidate.
  The configured endpoint selected at FINAL remains fixed during reconnect.
- `DONTWAIT` makes one admission attempt and pins no endpoint. If no ROUTER is eligible or none has
  write capacity, it returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` and a nonzero wait token,
  and Core retains no payload. After the WRITABLE record, the caller resubmits the same request and
  a ROUTER is selected again.
- The request timeout starts at local queue admission and does not start while a wait token is
  outstanding. When the submit-time pair terminates after admission, one
  `ZLINK_REQUEST_NOT_CONNECTED` completion arrives at once without waiting for the timeout, and the
  payload is not replayed.
- When the completion reservations shared by SEND wait tokens and REQUEST are exhausted, a REQUEST
  FINAL immediately returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN`, ID `0`, and no completion
  regardless of flags, and a DONTWAIT SEND returns `ZLINK_SUBMIT_OUT_OF_MEMORY` with `ENOMEM` and
  ID `0`.
- If the ROUTER sends multipart DATA before the REPLY for the same request,
  `ZLINK_POLLCOMPLETION` is not ready until the DATA `FINAL` part is dequeued. After the last DATA
  part, the REPLY appears as exactly one REQUEST completion, and its payload does not appear in DATA
  receive.
- If preceding DATA and local PAUSED delay the REPLY until the request timeout completes first,
  exactly one timeout completion is returned. A late REPLY that arrives after DATA is drained does
  not create a second completion.

**Receive**

- A non-blocking `zlink_recv_part()` call with no DATA available returns `ZLINK_RECV_NO_DATA` with `EAGAIN`.
- If `has_more_out_ == ZLINK_PART_MORE`, the next call returns the next part of the same record; `ZLINK_PART_FINAL` completes the record's receive sequence.
- On success, `source_rid_out_` is `NULL`; DEALER does not return inbound typed REQUEST records or
  requester replies through DATA receive.
- `NONE` snapshots `RCVTIMEO` on entry. Timeout, context termination, and socket shutdown follow the
  common recv result, errno, and output-invariance contract.

**Results and readiness**

- `ZLINK_POLLIN` means that a DATA record can be received; request completion is distinguished by
  `ZLINK_POLLCOMPLETION`.
- Observing `ZLINK_POLLOUT` after backpressure does not guarantee that the next submit will succeed.
- While an unread `ZLINK_COMPLETION_WRITABLE` record exists, `ZLINK_POLLOUT` and
  `ZLINK_POLLCOMPLETION` are level-held, and draining through `NO_DATA` clears them.

**Receive flow state**

- Setting `ZLINK_RECEIVE_FLOW_PAUSED` twice represents one pause; the second call succeeds without sending anything.
- A flow-state frame contains only the flow epoch scoped to the connection on which it was written. There is no public pair-ID or generation field and no `Zlink-Pair-Id` or `Zlink-Pair-Generation` wire property. Core applies the frame only to that connection by using internal connection identity.
- A frame whose identity does not match, including a frame from a replaced connection, is consumed internally without a public event and increments only `flow_state_stale_total`. A duplicate or regressing epoch on the same connection is not applied and is reported as `ZLINK_EVENT_FLOW_STATE_STALE` with `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH`.
- A peer that connects or reconnects while this socket is paused learns the pause without another call, and a socket that has never set a state sends nothing.
- DEALER-DEALER and DEALER-ROUTER both carry receive-flow control over the single Application
  connection. No control record appears in public receive or the socket-local completion queue.
- DEALER-ROUTER reconnect resends the current absolute state over one new Application connection;
  REPLY and FLOWSTATE from the previous connection ID or generation do not apply to the current
  connection.
- Clearing remote pause does not by itself make the next send succeed. A blocked non-blocking send continues to return `ZLINK_SUBMIT_BACKPRESSURED` with `errno == EAGAIN` and a wait token, and the remote RESUME publishes the WRITABLE record for that token.
- Remote PAUSE takes effect at the next message boundary—a message whose first byte has already reached the pipe or whose first part has already been accepted sends all remaining parts.
- The [Monitoring](../06-monitoring.en.md) status snapshot exposes the number of peers currently seen as paused, the numbers of applied pause and resume transitions, the number of frames rejected as stale, and the duration of the most recently completed pause.

<!-- zlink-nav:start -->
[Socket Index](README.en.md) | [Previous: XSUB](05-xsub.en.md) | [Next: ROUTER](07-router.en.md)
<!-- zlink-nav:end -->
