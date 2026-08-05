[한국어](11-dealer.ko.md) | English

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

## `zlink_dealer_recv_part`

Receives one part of a complete record from a DEALER socket, classified by type.

```c
uint8_t message_type;
uint64_t request_seq;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_dealer_recv_part(dealer, &message_type, &request_seq, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** Every output pointer is required: `message_type_out_` (a `uint8_t` holding a
`zlink_dealer_message_type_t` value), `request_seq_out_`, `part_out_` (already-initialized),
`has_more_out_`. `flags_` is `ZLINK_RECV_FLAGS_NONE` or `ZLINK_RECV_FLAGS_DONTWAIT`.

**Return and errno.** Returns `zlink_recv_result_t` — `ZLINK_RECV_OK` on success, with received
part ownership transferring to the caller (Message category). `ZLINK_RECV_NO_DATA` with `EAGAIN`
for a non-blocking call with nothing available.

**When to use.** Classify the record via `message_type_out_`: `ZLINK_DEALER_MESSAGE_RAW` (`0`,
`request_seq_out_ == 0`) is an ordinary raw multipart message with no request/reply envelope.
`ZLINK_DEALER_MESSAGE_REQUEST` (`1`, nonzero sequence) is a request this DEALER received — the
sequence is the reply token to pass to `zlink_dealer_reply_part`. `ZLINK_DEALER_MESSAGE_REPLY`
(`2`) and `ZLINK_DEALER_MESSAGE_ERROR_REPLY` (`3`) are reply records — in practice, replies and
terminal failures for work started via `zlink_dealer_request_part` are delivered through that
call's `zlink_reply_handler_fn` completion instead, not through this receive call. Every part of
one multipart record returns the same type and sequence; when `has_more_out_ ==
ZLINK_PART_MORE`, the next call continues the same record.

---

## `zlink_dealer_request_part`

Submits one asynchronous request payload, part at a time, expecting a reply via callback.

```c
zlink_dealer_request_part(dealer, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                           /*timeout_ms=*/3000, on_reply, userdata);
```

**Parameters.** For an intermediate part, pass `ZLINK_PART_MORE`, `timeout_ms_ == 0`,
`handler_ == NULL`, `userdata_ == NULL`. The final part uses `ZLINK_PART_FINAL` and a non-`NULL`
`handler_` (a `zlink_reply_handler_fn`); `timeout_ms_ == 0` on that final call uses the
`ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS` default instead of no timeout. `flags_` is
`ZLINK_SEND_FLAGS_NONE` or `ZLINK_SEND_FLAGS_DONTWAIT`.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on the final part means
exactly one completion will later reach `handler_`; a failed submit never invokes the handler.
Ownership of the callback's `parts_` and every message transfers to the callback, which releases
them exactly once. `zlink_request_result_t` (delivered to the callback, not returned here)
identifies timeout and other terminal outcomes.

**When to use.** Use this instead of `zlink_send_part` (PAIR category) when the message needs a
tracked reply. As with any `*_part` send family, a sequence from `ZLINK_PART_MORE` through
`ZLINK_PART_FINAL` must complete on the same handle without interleaving a different send-helper
family; a failure at any point atomically discards every staged part in that record (nothing
becomes visible to the peer), still consumes the failed call's `part_`, and creates no request
sequence or handler invocation — retry the whole record from its first part using retained
copies.

---

## `zlink_dealer_reply_part`

Sends a reply part for a request record this DEALER received.

```c
zlink_dealer_reply_part(dealer, request_seq, &reply_part, ZLINK_PART_FINAL);
```

**Parameters.** `request_seq_` must be the nonzero reply token `zlink_dealer_recv_part` returned
for that request, on the same socket. `part_`/`part_flag_` follow the same multipart rules as
other send families — a multipart reply reuses the same token for every part.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on a successful final
part completes the reply for that token, which then cannot be reused. On a reply-sequence
failure, the token stays valid until either a successful `ZLINK_PART_FINAL` or request-lifecycle
termination, so a retained complete reply can be resubmitted from its first part.

**When to use.** Use this to answer a record classified as `ZLINK_DEALER_MESSAGE_REQUEST` by
`zlink_dealer_recv_part`. `ZLINK_POLLIN` (Polling and pollers category) means a raw or
request/reply record can be received; `ZLINK_POLLOUT`/`zlink_send_ready_handler` (Socket
lifecycle category) mean retrying a backpressured submit is worth attempting, not that it will
succeed.

---

See the [DEALER specification](../spec/core/socket/06-dealer.en.md) for the full rationale. Raw
one-way send uses `zlink_send_part` (PAIR category) — not repeated here.
