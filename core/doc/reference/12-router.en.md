
[Reference index](README.en.md)

# 12. ROUTER

An asynchronous raw socket that manages multiple peer pipes on one socket and selects a send
target by routing ID. Handles ordinary directed messages and request/reply records through
logical routing IDs. The exact signatures are owned by the
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

Sends one ordinary directed raw multipart part to a specific peer by routing ID — it creates no
reply token and waits for no reply.

```c
zlink_send_part_rid(router, &target_rid, &part, ZLINK_SEND_FLAGS_NONE,
                    ZLINK_PART_FINAL, NULL, NULL);
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

## `zlink_request_part` (ROUTER)

Submits one request payload part to a specific logical ROUTER peer. Reply or terminal result is
pulled from the socket-local completion queue.

```c
zlink_completion_id_t id = 0;
zlink_request_part(router, &peer_rid, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                   ZLINK_PART_FINAL, 3000, user_context, &id);
```

**Parameters.** `target_router_rid_or_null_` is the non-NULL logical RID of a ROUTER peer. MORE
requires timeout `0` and NULL context. FINAL accepts an explicit reply timeout; `0` selects
`ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS`. A successful FINAL always returns a nonzero ID.

**Return and errno.** Successful FINAL reserves exactly one REQUEST completion. A failure before
admission returns ID `0` and produces none. The completion carries `request_result` and, for OK,
the reply multipart; release it with `zlink_completion_close()`.

**When to use.** Use this instead of `zlink_send_part_rid` when the directed message needs a
tracked reply from a specific peer.

---

## Request completion receive

REQUEST replies, timeouts, and terminal results never appear as DATA from
`zlink_router_recv_part()`.

```c
zlink_completion_t completion = {0};
completion.struct_size = sizeof(completion);
zlink_completion_recv(router, &completion, ZLINK_RECV_FLAGS_NONE);
/* Match completion.completion_id or completion.user_context. */
zlink_completion_close(&completion);
```

**Parameters.** The output is caller-owned, has exact `struct_size`, and is otherwise empty.
`flags_` is NONE or DONTWAIT. REQUEST OK owns a contiguous reply array until close.

**Return and errno.** Empty DONTWAIT receive returns `ZLINK_RECV_NO_DATA` with `EAGAIN`.
Successfully received records must all be closed, including payload-free terminal records.

**When to use.** Drain after `ZLINK_POLLCOMPLETION` until NO_DATA. Use the completion ID or
context for correlation; completion order need not match submit order or per-target wire order.

---

## Logical routed target

The public send target is a logical routing ID, not a physical connection identifier or
generation. Core resolves the current route while admitting FINAL.

```c
zlink_send_part_rid(router, &peer_rid, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                    ZLINK_PART_FINAL, user_context, &completion_id);
```

**Parameters.** `peer_rid` is the application-visible identifier copied from receive metadata or
otherwise configured by the application. It contains no public physical-pair fields.

**Return and errno.** A missing logical RID follows the ROUTER mandatory contract. If DONTWAIT
admission is pending, a successful submit returns a nonzero completion ID rather than exposing or
pinning the selected pipe.

**When to use.** Store logical RIDs when application routing requires stable identity. Do not
cache monitor connection IDs as send capabilities; reconnect may replace the physical connection
while the logical target remains the same.

---

## Pending same-RID admission

A DONTWAIT FINAL that cannot enter the local send queue may be retained by Core against its
logical target.

```c
zlink_completion_id_t id = 0;
zlink_submit_result_t result = zlink_send_part_rid(
    router, &target_rid, &part, ZLINK_SEND_FLAGS_DONTWAIT,
    ZLINK_PART_FINAL, user_context, &id);
```

**Parameters.** The target RID and flags are fixed for the complete multipart. Core consumes
every part on success and failure; retain a separate full-record copy before submitting if the
application needs recovery from a synchronous failure.

**Return and errno.** ID `0` means immediate local admission and no completion. A nonzero ID means
Core owns the pending record and later produces one SEND completion. Pending pool or completion
slot exhaustion rejects synchronously with ID `0` and no completion.

**When to use.** Do not place an accepted pending record into a caller retry queue. Core waits for
the same logical RID across transient reconnect. After admission it neither replays the payload
nor changes `ZLINK_SEND_ADMITTED` into a peer-delivery acknowledgement.

---

## `zlink_router_recv_part`

Receives one part of a complete raw record — either an ordinary directed message or an incoming
request.

```c
const zlink_routing_id_t *source_rid;
zlink_reply_token_t reply_token;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_router_recv_part(router, &source_rid, &reply_token, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** Every output pointer is required. `source_rid_out_` is a socket-owned view — copy
it if it must outlive the next data-receive entry on this socket; that entry invalidates the
previous view whether it succeeds or fails. `reply_token_out_` is opaque.

**Return and errno.** Returns `zlink_recv_result_t` — `ZLINK_RECV_OK` on success, with received
part ownership transferring to the caller. `ZLINK_RECV_NO_DATA` with `EAGAIN` for a non-blocking
call with nothing available.

**When to use.** Distinguish the record kind from the token: token `0` is ordinary DATA; a nonzero
token is a received REQUEST and is passed with the same RID to `zlink_reply_part()`. Replies and
terminal failures for work started via `zlink_request_part()` appear in the completion queue,
not here. When `has_more_out_ ==
ZLINK_PART_MORE`, the next call continues the same record.

---

## Received RID and token lifetime

All parts of one ROUTER multipart return the same logical RID and reply token. Neither value
exposes a physical transport-pair identity.

```c
/* Copy the RID before the next data receive on this router. */
zlink_routing_id_t rid_copy = *source_rid;
zlink_reply_token_t token_copy = reply_token;
```

**Parameters.** The RID bytes are a borrowed socket-owned view. The token is a scalar opaque
capability bound to the source RID and the ROUTER that produced it.

**Return and errno.** Completion, monitor, and poller calls do not invalidate the RID view; the
next DATA receive entry on this same socket does. Receive on another socket does not invalidate it.

**When to use.** Copy the RID immediately if processing can issue another receive first. Preserve
the token unchanged only for replying to that REQUEST; do not compare its size or infer a wire
sequence from it.

---

## `zlink_reply_part`

Sends a reply part for a request record this ROUTER received.

```c
zlink_reply_part(router, &peer_rid, reply_token, &reply_part, ZLINK_PART_FINAL);
```

**Parameters.** `source_rid_` and `reply_token_` must be exactly the values
`zlink_router_recv_part()` returned for that REQUEST. A multipart reply reuses both for every part.

**Return and errno.** Returns `zlink_submit_result_t` — a successful final part completes the
reply. On a reply-sequence failure, the token/peer-RID pair stays valid until a successful final
part or request-lifecycle termination, so a retained complete reply can be resubmitted from its
first part.

**When to use.** Use this to answer a REQUEST that `zlink_router_recv_part()` surfaced with a
nonzero token. DEALER-ROUTER replies use the Application lane and may be backpressured behind DATA;
ROUTER-ROUTER replies use the Completion lane.

---

## Related general-purpose function

`zlink_disconnect_rid()` (declared alongside `zlink_connect`/`zlink_disconnect` in
`socket/api.h`, specified in the [socket README](../spec/core/socket/README.en.md)) requests
termination of the current peer connection selected by logical RID. Success means the request was
accepted, not that remote shutdown has completed. If duplicate RIDs make lookup ambiguous, the
call reports conflict rather than exposing a physical pair selector.

---

See the [ROUTER specification](../spec/core/socket/07-router.en.md) for the full rationale.
