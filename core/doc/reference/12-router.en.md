
[Reference index](README.en.md)

# 12. ROUTER

An asynchronous raw socket that manages multiple peer pipes on one socket and selects a send
target by routing ID. Handles ordinary directed messages and request/reply records, either by
routing ID or pinned to one exact transport pair. The exact signatures are owned by the
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

## `zlink_router_request_transport_pair_part`

Submits an asynchronous request to a peer through one specified transport pair only.

```c
zlink_router_request_transport_pair_part(router, &peer_rid, target.transport_pair_id,
                                          target.transport_pair_generation, &part,
                                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                                          /*timeout_ms=*/3000, on_reply, userdata);
```

**Parameters.** `peer_rid_`, `transport_pair_id_`, and `transport_pair_generation_` identify the
same exact pipe as `zlink_send_part_transport_pair` (this category); the remaining parameters
follow `zlink_router_request_part`'s intermediate/final-part convention.

**Return and errno.** Returns `zlink_submit_result_t`, with the same exact-target validation, no
rerouting, and rollback rules as `zlink_send_part_transport_pair`. Core registers the pending
correlation before the request envelope becomes visible on the wire; a failed final submit removes
that pending entry and the completion reservation without invoking `handler_`.

**When to use.** Use this instead of `zlink_router_request_part` when the tracked request must go
through the exact physical connection already selected via `zlink_select_routed_submit_target`
(this category), rather than letting Core reselect a current route for that routing ID.

---

## `zlink_select_routed_submit_target`

Snapshots one exact routed submit target without claiming pipe credit, for later exact-target
submits.

```c
zlink_routed_submit_target_t target;
zlink_select_routed_submit_target(router, &peer_rid, &target);
```

**Parameters.** `router_rid_or_null_` is required and non-`NULL` on ROUTER, identifying the peer
whose exact admitted route is snapshotted. (On DEALER, the same function requires `NULL` and
commits one weighted selection instead — see DEALER category.) `target_out_` receives the
`zlink_routed_submit_target_t` (peer routing ID, transport pair ID, and transport pair generation)
to hold in binding-owned pending state before a DONTWAIT exact submit.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on success. The
selection is a value snapshot: it does not reserve the pipe or its credit, so a later exact submit
using `target_out_` can still fail if the pipe detaches or its generation changes meanwhile.

**When to use.** Use this to obtain the exact pipe identity a binding needs before submitting
through `zlink_send_part_transport_pair`, `zlink_router_request_transport_pair_part`
(both this category), or DEALER's exact-target sends (DEALER category). Prefer this over
`zlink_send_part_rid`/`zlink_router_request_part` when the application must pin a multipart
attempt to the exact physical connection it already observed, rather than letting Core reselect a
current route for that routing ID.

---

## `zlink_send_part_transport_pair`

Submits one ROUTER raw part only through the specified transport pair — no rerouting to another
connection with the same routing ID.

```c
zlink_send_part_transport_pair(router, &target_rid, target.transport_pair_id,
                                target.transport_pair_generation, &part,
                                ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `target_rid_`, `transport_pair_id_`, and `transport_pair_generation_` must
describe the same admitted peer — typically obtained from `zlink_select_routed_submit_target` or
from a monitor event's pair identity. `flags_`/`part_flag_` follow the same conventions as
`zlink_send_part_rid`.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on success.
`ZLINK_SUBMIT_BACKPRESSURED` when the exact pipe is at HWM; `ZLINK_SUBMIT_NOT_CONNECTED` after
detach or for a stale generation. Neither case reselects another pipe for the same routing ID. Once
the first part is accepted, the exact-pipe fence holds through `ZLINK_PART_FINAL`, and any failure
rolls back the entire staged record.

**When to use.** Use this instead of `zlink_send_part_rid` when the application must pin a
multipart send to the exact physical connection it already selected — for example, after observing
a peer reconnect under the same routing ID and needing to avoid silently rerouting to the new
connection.

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

## `zlink_router_recv_part_v2`

Receives one raw record part with the same contract as `zlink_router_recv_part`, additionally
returning the source transport pair identity.

```c
const zlink_routing_id_t *source_rid;
uint64_t request_seq, pair_id, pair_generation;
zlink_msg_t part;
zlink_msg_init(&part);
zlink_part_flag_t has_more;
zlink_router_recv_part_v2(router, &source_rid, &request_seq, &pair_id, &pair_generation,
                           &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** Same required output pointers as `zlink_router_recv_part`, plus
`transport_pair_id_out_` and `transport_pair_generation_out_`.

**Return and errno.** Same as `zlink_router_recv_part`. Every part of one multipart record returns
the same routing ID, request sequence, pair ID, and pair generation.

**When to use.** Use this instead of `zlink_router_recv_part` when the caller needs the exact
transport pair the record arrived on — for example, to reply or follow up through
`zlink_send_part_transport_pair`/`zlink_router_request_transport_pair_part` (this category)
without re-resolving the routing ID to a possibly different current pipe.

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

## Related general-purpose function

`zlink_disconnect_transport_pair` (declared alongside `zlink_connect`/`zlink_disconnect` in
`socket/api.h`, specified in the [socket README](../spec/core/socket/README.en.md)) disconnects
the exact transport pair identified by a monitor event's pair id and generation, without affecting
another connection that shares the same peer routing id. It applies to any socket type that
exposes transport pairs, not just ROUTER, so its narrative home is Socket lifecycle category
(`03-socket-lifecycle.en.md`, alongside `zlink_disconnect`/`zlink_disconnect_rid`) rather than this
file — noted here only because it is the natural counterpart to
`zlink_send_part_transport_pair`/`zlink_router_request_transport_pair_part` above.

---

See the [ROUTER specification](../spec/core/socket/07-router.en.md) for the full rationale.
