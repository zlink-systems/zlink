[한국어](12-router.ko.md) | English

[Reference index](README.en.md)

# 12. ROUTER

An asynchronous raw socket that manages multiple peer pipes on one socket and selects a send
target by routing ID. Handles ordinary directed messages, request/reply records, and a separate
completion-control channel. The exact signatures are owned by the
[ROUTER specification](../spec/core/socket/07-router.en.md).

---

## `zlink_set_router_option` / `zlink_get_router_option`

Sets or reads a ROUTER-specific option.

```c
int mandatory = 1;
zlink_set_router_option(router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof(mandatory));
```

**Parameters.** `option_` is `ZLINK_ROUTER_OPT_MANDATORY` (`int` 0/1, default `1` — when `1`, a
directed submit to a routing ID with no connected pipe fails with
`ZLINK_SUBMIT_NOT_CONNECTED`), `ZLINK_ROUTER_OPT_PROBE` (`int` 0/1, default `0` — same meaning
as DEALER's probe option), `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID` (a variable-length byte string,
set-only — sets the local alias for the pipe the *next* `zlink_connect` will create; set it
before each connect), `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` (nonnegative `int` ms, default
`5000`), or `ZLINK_ROUTER_OPT_WEIGHT` (`int` `0..10000`, default `100`).

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success. Use
`zlink_set_option`/`zlink_get_option` (Socket options and identity category) for HWM, reconnect,
and timeout options that aren't ROUTER-specific.

**When to use.** Set `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID` immediately before a `zlink_connect`
call when the application needs a predictable local alias for that specific outbound pipe,
distinct from the socket's own routing identity (`zlink_set_routing_id`, Socket options and
identity category).

---

## `zlink_send_part_rid`

Sends one ordinary directed raw multipart part to a specific peer by routing ID — no request
sequence or reply expected.

```c
zlink_send_part_rid(router, &target_rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `target_rid_` identifies the destination peer; every part of the same multipart
message uses the same target. `flags_` is `ZLINK_SEND_FLAGS_NONE` or
`ZLINK_SEND_FLAGS_DONTWAIT`.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on success.
`ZLINK_SUBMIT_NOT_CONNECTED` if `ZLINK_ROUTER_OPT_MANDATORY` is `1` and no pipe is connected to
`target_rid_`.

**When to use.** Use this for a one-way directed message where no reply tracking is needed. As
with every `*_part` send family on ROUTER, a sequence from `ZLINK_PART_MORE` through
`ZLINK_PART_FINAL` must complete on the same handle without interleaving a different send-helper
family or a different routing ID; a failure atomically discards every staged part in that record.

---

## `zlink_router_request_part`

Submits an asynchronous request payload to a specific peer, part at a time, expecting a reply via
callback.

```c
zlink_router_request_part(router, &peer_rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                           /*timeout_ms=*/3000, on_reply, userdata);
```

**Parameters.** `peer_rid_` is the request target. The intermediate/final part convention
matches DEALER's `zlink_dealer_request_part` (DEALER category): intermediate parts pass
`ZLINK_PART_MORE`/`timeout_ms_ == 0`/`handler_ == NULL`/`userdata_ == NULL`; the final part uses
`ZLINK_PART_FINAL` and a non-`NULL` `handler_`, with `timeout_ms_ == 0` selecting the
`ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` default.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on the final part means
exactly one completion later reaches `handler_`; a failed submit never invokes it. Ownership of
the callback's `parts_` transfers to the callback.

**When to use.** Use this instead of `zlink_send_part_rid` when the directed message needs a
tracked reply from a specific peer.

---

## `zlink_router_recv_part`

Receives one part of a complete raw record — either an ordinary directed message or an incoming
request.

```c
const zlink_routing_id_t *source_rid;
uint64_t request_seq;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_router_recv_part(router, &source_rid, &request_seq, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** Every output pointer is required. `source_node_rid_out_` is a thread-local view
owned by Core — copy it if it must outlive the next raw receive call on this thread; the next
call invalidates the previous view.

**Return and errno.** Returns `zlink_recv_result_t` — `ZLINK_RECV_OK` on success, with received
part ownership transferring to the caller. `ZLINK_RECV_NO_DATA` with `EAGAIN` for a non-blocking
call with nothing available.

**When to use.** Distinguish the record kind from the output combination: `request_seq_out_ == 0`
is an ordinary raw multipart message; a nonzero value is a received request whose sequence is the
reply token to pass to `zlink_router_reply_part`, paired with the same `source_node_rid_out_`.
Replies and terminal failures for work started via `zlink_router_request_part` are delivered
through that call's callback instead of appearing here. When `has_more_out_ ==
ZLINK_PART_MORE`, the next call continues the same record.

---

## `zlink_router_reply_part`

Sends a reply part for a request record this ROUTER received.

```c
zlink_router_reply_part(router, &peer_rid, request_seq, &reply_part, ZLINK_PART_FINAL);
```

**Parameters.** `peer_rid_` and `request_seq_` must be exactly the values `zlink_router_recv_part`
returned for that request record. A multipart reply reuses both for every part.

**Return and errno.** Returns `zlink_submit_result_t` — a successful final part completes the
reply. On a reply-sequence failure, the token/peer-RID pair stays valid until a successful final
part or request-lifecycle termination, so a retained complete reply can be resubmitted from its
first part.

**When to use.** Use this to answer a request `zlink_router_recv_part` surfaced with a nonzero
sequence.

---

## `zlink_router_completion_control_handler` / `zlink_router_completion_control_part`

Registers a handler for, and sends, a bounded control record on the Completion connection —
separate from ordinary directed messages and requests, which stay on the Application connection.

```c
zlink_router_completion_control_handler(router, on_control, userdata);
// ...
zlink_router_completion_control_part(router, &peer_rid, &control_part, ZLINK_PART_FINAL);
```

**Parameters.** `handler_` is a `zlink_completion_control_handler_fn`; each socket has one
handler, and registering again replaces it. Core does not interpret the record's contents — it
defines no command kind, allowlist, or application meaning for the payload.

**Return and errno.** `handler` registration returns `zlink_handler_result_t` —
`ZLINK_HANDLER_INVALID_ARGUMENT` for a `NULL` handler, `ZLINK_HANDLER_NOT_SUPPORTED` for a
non-ROUTER socket. Sending returns `zlink_submit_result_t`, following the same part-sequencing
and consume-on-every-result rules as other send families (DEALER category); the Completion
connection has a finite byte HWM, so a `ZLINK_SUBMIT_BACKPRESSURED` result means retry the whole
record from its first part, using retained copies, after send-ready. A record with no registered
handler is discarded.

**When to use.** Use this only for infrastructure-level signaling that must not compete with
application message/request traffic on the same connection — it creates no new socket or
connection. The handler runs when the completion owner processes the connection, so a
`ZLINK_POLLCOMPLETION` poller (Polling and pollers category) can receive controls without an
application receive call. `source_rid_` in the callback is valid only until the callback returns.
Closing the socket while the callback is running fails with `ZLINK_CLOSE_BUSY`/`EBUSY` — retry
after the callback returns.

---

See the [ROUTER specification](../spec/core/socket/07-router.en.md) for the full rationale.
