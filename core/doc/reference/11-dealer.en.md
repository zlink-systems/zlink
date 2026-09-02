
[Reference index](README.en.md)

# 11. DEALER

An asynchronous raw socket that fair-queues inbound messages and selects an outbound peer by
round-robin/weight. The same socket handles ordinary raw messages and request/reply records.
DEALER shares `zlink_send_part` with PAIR (PAIR category) for the raw send side unchanged; this
category covers DEALER's own options and its request/reply data plane. The exact signatures are
owned by the [DEALER specification](../spec/core/socket/06-dealer.en.md).

---

## `zlink_set_dealer_option` / `zlink_get_dealer_option`

Sets or reads a DEALER-specific option.

```c
int weight = 300;
zlink_set_dealer_option(dealer, ZLINK_DEALER_OPT_WEIGHT, &weight, sizeof(weight));
```

**Parameters.** `option_` is `ZLINK_DEALER_OPT_PROBE` (`int` 0/1, default `0` — send an empty
raw message on connect so the peer observes the connection and routing ID),
`ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS` (nonnegative `int` ms, default `5000` — the default used
by a request call when `timeout_ms_ == 0`), or `ZLINK_DEALER_OPT_WEIGHT` (`int` `0..10000`,
default `100` — this DEALER's advertised weight; out-of-range is rejected, never clamped).

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success. Use
`zlink_set_option`/`zlink_get_option` (Socket options and identity category) for HWM, reconnect,
and timeout options that aren't DEALER-specific.

**When to use.** Tune `ZLINK_DEALER_OPT_WEIGHT` to bias outbound peer selection: a candidate is a
connected peer with positive advertised weight (weight `0` excludes it from the candidate set —
if every known peer is weight `0`, a submit may fail with `ZLINK_SUBMIT_NOT_ADMITTED`).
Selection runs a running-value algorithm per candidate (add weight, pick the largest running
value — ties broken by smallest routing-ID identifier — then subtract the candidate set's total
weight from the winner) that spreads consecutive sends according to the configured ratio rather
than grouping each candidate's share into one run; two processes with the same peers and weights
produce the same order as long as identifiers are distinct. A candidate that can't accept a write
right now (backpressure) is skipped for that message without losing its running value, and
rejoins once it reports capacity again; a message rejected for exceeding a size limit is not
retried against another candidate.

---

## `zlink_send_part` (DEALER)

Submits one ordinary DATA part. Core selects one compatible positive-weight logical route when
the FINAL record is submitted.

```c
zlink_completion_id_t id = 0;
zlink_send_part(dealer, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                ZLINK_PART_FINAL, user_context, &id);
```

**Parameters.** A multipart uses this same function and flags from MORE through FINAL.
`user_context_` may be non-NULL only for DONTWAIT FINAL. The optional completion ID is zero for
MORE, blocking NONE, and immediate DONTWAIT admission.

**Return and errno.** `NONE FINAL` waits for local admission within `SNDTIMEO`. DONTWAIT returns
immediately; if Core retains the record before admission, it returns a nonzero ID and later one
SEND completion. A failed part consumes the input and rolls back the whole staged record.

**When to use.** Use this for one-way DATA. Once Core retains a DONTWAIT record, Core owns retry
for the configured endpoint selected at FINAL. Do not enqueue or resubmit the same payload in the
application. Admission completion does not confirm peer delivery.

---

## `zlink_recv_part` (DEALER)

Receives one part of an ordinary DATA record from a DEALER socket.

```c
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_recv_part(dealer, NULL, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `source_rid_out_` is optional and is `NULL` for DEALER. `part_out_` must be an
initialized message and `has_more_out_` receives MORE or FINAL. `flags_` is
`ZLINK_RECV_FLAGS_NONE` or `ZLINK_RECV_FLAGS_DONTWAIT`.

**Return and errno.** Returns `zlink_recv_result_t` — `ZLINK_RECV_OK` on success, with received
part ownership transferring to the caller (Message category). `ZLINK_RECV_NO_DATA` with `EAGAIN`
for a non-blocking call with nothing available.

**When to use.** DEALER receives only ordinary DATA here. Replies, timeouts, and terminal results
for submitted requests appear only as REQUEST records from `zlink_completion_recv()`, never as
DATA. When `has_more_out_ == ZLINK_PART_MORE`, the next call continues the same DATA record.

---

## `zlink_request_part` (DEALER)

Submits one request payload part at a time. Its reply or terminal result is pulled from the
socket-local completion queue.

```c
zlink_completion_id_t id = 0;
zlink_request_part(dealer, NULL, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                   ZLINK_PART_FINAL, 3000, user_context, &id);
```

**Parameters.** DEALER passes `NULL` as the target because Core selects one ready ROUTER logical
route. MORE requires timeout `0` and NULL context. FINAL accepts an explicit reply timeout;
`0` selects `ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS`. A successful FINAL always returns a nonzero ID.

**Return and errno.** A successful FINAL reserves correlation and exactly one REQUEST completion.
Pre-admission failure returns ID `0` and creates no completion. On success,
`zlink_completion_recv()` returns `request_result` and, for OK, the reply multipart. Close every
record with `zlink_completion_close()`.

**When to use.** Use this instead of `zlink_send_part` (PAIR category) when the message needs a
tracked reply. As with any `*_part` send family, a sequence from `ZLINK_PART_MORE` through
`ZLINK_PART_FINAL` must complete on the same handle without interleaving a different send-helper
family; a failure at any point atomically discards every staged part in that record (nothing
becomes visible to the peer), still consumes the failed call's `part_`, and creates no request
sequence or completion — retry the whole record from its first part using retained
copies.

---

## `zlink_completion_recv` (DEALER)

Pulls one pending-send or request terminal record from the DEALER completion queue.

```c
zlink_completion_t completion = {0};
completion.struct_size = sizeof(completion);
zlink_recv_result_t rc = zlink_completion_recv(
    dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
if (rc == ZLINK_RECV_OK)
    zlink_completion_close(&completion);
```

**Parameters.** The output must have exact `struct_size` and otherwise be empty. `flags_` is NONE
or DONTWAIT. Use `completion_id` or `user_context` to find the operation; resolver order is not
submit order.

**Return and errno.** DONTWAIT on an empty queue returns `ZLINK_RECV_NO_DATA` and `EAGAIN`.
Successful REQUEST records own their reply array until `zlink_completion_close()`.

**When to use.** After `ZLINK_POLLCOMPLETION`, drain repeatedly through NO_DATA. This queue is
separate from DATA receive even though a DEALER-ROUTER REPLY shares the physical Application FIFO
with preceding DATA and can therefore be delayed behind it.

---

## DEALER responder boundary

DEALER is a request originator, not a typed REQUEST responder. It exposes no DEALER reply-part
function and `zlink_recv_part()` never returns a reply token.

```c
/* Responders use a ROUTER socket and the token from zlink_router_recv_part(). */
zlink_reply_part(router, source_rid, reply_token, &reply_part, ZLINK_PART_FINAL);
```

**Parameters.** `reply_token` is an opaque socket-owned capability returned only by ROUTER
request receive. It is not a wire sequence and must not be synthesized or interpreted.

**Return and errno.** `zlink_reply_part()` validates the source RID, token, and owning ROUTER.
Only successful FINAL consumes the token; a failed attempt leaves it available for a complete
retry while the request lifecycle still exists.

**When to use.** Pair DEALER request submission with a ROUTER responder. `ZLINK_POLLIN` on DEALER
means DATA can be received; `ZLINK_POLLCOMPLETION` means a SEND or REQUEST completion can be
drained. Neither readiness bit guarantees peer delivery or that a later submit will succeed.

---

See the [DEALER specification](../spec/core/socket/06-dealer.en.md) for the full rationale. Raw
one-way send uses `zlink_send_part` (PAIR category) — not repeated here.
