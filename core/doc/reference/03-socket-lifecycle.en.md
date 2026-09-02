
[Reference index](README.en.md)

# 03. Socket lifecycle

This category covers the entry points common to every raw socket type: creation, endpoint
lifecycle (bind/connect/disconnect), closing, blocking or completion-backed send, and receive flow control.
Per-socket-type options and data-plane operations live in their own categories (PAIR/PUB/SUB/
XPUB/XSUB/DEALER/ROUTER/STREAM below). The exact signatures are owned by the
[Socket common specification](../spec/core/socket/README.en.md).

---

## `zlink_socket`

Creates a new socket within a context. The prerequisite for every other entry in this category
and the per-socket-type categories.

```c
void *s = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
```

**Parameters.** `context_` is a context from `zlink_ctx_new`. `type_` is one of the
`ZLINK_SOCKET_*` constants (`PAIR`/`PUB`/`SUB`/`DEALER`/`ROUTER`/`XPUB`/`XSUB`/`STREAM`) —
`ZLINK_SOCKET_ANY` is a wildcard for filter APIs and is not itself creatable.

**Return and errno.** Returns a socket handle on success, or `NULL` on failure with `errno` set —
`EINVAL` for an invalid type, `EMFILE` if the maximum socket count has been reached, `ETERM` if
the context was terminated.

**When to use.** Call this once per socket your application needs. The receive mode is fixed by
`type_`: `PAIR`/`DEALER`/`SUB`/`XSUB` use part receive, `ROUTER` uses
`zlink_router_recv_part()`, and only `STREAM` selects RAW part receive or PACKET receive before
its first bind or connect (see the STREAM category). The socket must be closed with
`zlink_close` before the context terminates.

---

## `zlink_close`

Closes a socket and releases its resources.

```c
zlink_close_result_t result = zlink_close(s);
```

**Parameters.** Only the socket handle.

**Return and errno.** Returns `zlink_close_result_t` — `ZLINK_CLOSE_OK` on success. `ENOTSOCK`
if the handle is not a valid socket, `EBUSY` if an operation is in-flight on the handle from
another thread.

**When to use.** Call this to release a socket you no longer need. Outstanding send-queue
messages are discarded or sent depending on `ZLINK_OPT_LINGER` (Socket options and identity
category). Public handles follow a tiered thread-safety contract — hot-path `send` may be called
concurrently, control-path calls (bind/connect/option/monitor) serialize for correctness, and
`close` uses a stricter fail-fast gate: once accepted, new API entry on the same handle fails
with `ESHUTDOWN`.

---

## `zlink_bind` / `zlink_unbind`

Binds a socket to a local endpoint for others to connect to, or removes a previously established
binding.

```c
zlink_bind_result_t bound = zlink_bind(s, "tcp://*:5555");
// ...
zlink_unbind(s, "tcp://*:5555");
```

**Parameters.** `addr_` is `transport://address` — supported transports are `tcp://`, `inproc://`
(in-process), `ipc://` (POSIX only), `ws://` (WebSocket), and `tls://` (TLS-encrypted TCP). A
socket can bind to multiple endpoints. Port `0` for TCP requests an ephemeral port.

**Return and errno.** `bind` returns `zlink_bind_result_t` — `ZLINK_BIND_OK` on success,
`EADDRINUSE` if the address is in use, `EADDRNOTAVAIL` if the interface doesn't exist,
`EPROTONOSUPPORT` for an unsupported transport. `unbind` returns `zlink_connect_result_t` —
`ZLINK_CONNECT_OK` on success.

**When to use.** Use `bind` for the listening side of a connection. When binding to an ephemeral
port (`0`), retrieve the actual bound endpoint with `ZLINK_OPT_LAST_ENDPOINT`
(`zlink_get_option`, Socket options and identity category). Use `unbind` to stop listening on a
specific endpoint without closing the socket.

---

## `zlink_connect` / `zlink_disconnect` / `zlink_disconnect_rid`

Connects a socket to a remote endpoint, or removes a connection by endpoint or by peer routing
ID.

```c
zlink_connect(s, "tcp://peer-host:5555");
// ...
zlink_disconnect(s, "tcp://peer-host:5555");
// or, by routing id instead of endpoint:
zlink_disconnect_rid(s, &peer_rid);
```

**Parameters.** `connect_`/`disconnect_` take the same endpoint format as `bind`.
`disconnect_rid` takes a `zlink_routing_id_t *peer_rid_`, which must not be empty.

**Return and errno.** All three return `zlink_connect_result_t` — `ZLINK_CONNECT_OK` on success.
`disconnect_rid` additionally maps a missing target to `ZLINK_CONNECT_NOT_FOUND`, an ambiguous
duplicate routing ID to `ZLINK_CONNECT_CONFLICT`, and a lifecycle ownership conflict to
`ZLINK_CONNECT_BUSY`.

**When to use.** Use `connect` for the connecting side; a socket can connect to multiple
endpoints, and the library reconnects automatically if a peer becomes unavailable. Use
`disconnect` when the endpoint address is known. Use `disconnect_rid` instead when only the
peer's routing ID is known — `ROUTER` and `STREAM` look it up in their routing maps directly (for
`STREAM`, `peer_rid_` must be the 4-byte connection routing ID); other socket types scan the
current connected-pipe snapshot, and the call fails if more than one pipe shares that routing ID.
A successful `disconnect_rid` return does not mean the remote peer has already processed the
termination.

---

## `zlink_send_part` / `zlink_send_part_rid` / `zlink_completion_recv`

The ordinary part-send functions select blocking local admission or completion-backed pending
admission with the `flags_` argument. `NONE FINAL` blocks for at most the socket's snapshotted
`ZLINK_OPT_SNDTIMEO`; `DONTWAIT FINAL` returns immediately and, only when Core retains the record,
returns a nonzero socket-local completion ID.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_size);
memcpy(zlink_msg_data(&part), payload, payload_size);

zlink_completion_id_t id = 0;
zlink_submit_result_t r = zlink_send_part(
    s, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
    user_context, &id);

if (r == ZLINK_SUBMIT_OK && id != 0) {
    zlink_completion_t completion = {0};
    completion.struct_size = sizeof(completion);
    if (zlink_completion_recv(s, &completion, ZLINK_RECV_FLAGS_NONE)
        == ZLINK_RECV_OK) {
        /* Match completion_id or user_context before consuming the result. */
        zlink_completion_close(&completion);
    }
}
```

**Parameters.** `zlink_send_part()` lets Core choose a logical target; routed sockets use
`zlink_send_part_rid()` with a logical RID. A physical pair ID or generation is not a public
target. Every part call consumes `part_` on success and failure. `MORE` stages one part and
`FINAL` submits the complete record. A sequence uses one function family, target, and flag.
`user_context_` may be non-NULL only for a DONTWAIT `FINAL`, and Core echoes but never reads or
frees it. The optional ID output is zeroed before validation.

**Return and errno.** A successful `NONE FINAL` or immediately admitted `DONTWAIT FINAL` returns
`ZLINK_SUBMIT_OK` with ID `0` and creates no completion. If Core retains a DONTWAIT record before
admission, the call returns `ZLINK_SUBMIT_OK` with a nonzero ID and later creates exactly one SEND
completion. Validation, no-target, `SNDTIMEO`, pending-limit, or completion-slot failure returns
synchronously with ID `0` and no completion. `zlink_completion_recv()` is available only on raw
`PAIR`/`DEALER`/`ROUTER`/`STREAM`; every successfully received SEND or REQUEST record must be
closed with `zlink_completion_close()`.

**Pending ownership and limits.** Once a successful DONTWAIT submit returns a nonzero ID, Core
owns the complete record and its pre-admission retry. The application must not build a second
retry queue or resubmit that payload. Core retries only the same logical PAIR route, configured
DEALER endpoint, or routed RID across transient reconnect. `ZLINK_OPT_PENDING_MAX_MSGS` and
`ZLINK_OPT_PENDING_MAX_BYTES` bound the socket-local pool shared by DONTWAIT SEND and REQUEST;
`0` means unlimited. They are supported only by PAIR, DEALER, ROUTER, and STREAM. The former
Legacy send-scoped pending-limit names are not used in 0.16.0.

**Admission is not delivery.** ID `0` and `ZLINK_SEND_ADMITTED` mean local send-queue admission.
They do not confirm peer receipt, create a delivery acknowledgement, or cause replay after a
later disconnect. Use request/reply when the application needs a correlated peer response.

### Cancellation boundary

Core exposes no operation-cancel API after successful submit; a completion ID is correlation,
not a control handle. Before Core submit, an application or language binding can cancel without
calling Core. A Framework-owned queue may also remove work before submit. After Core owns the
record, cancellation can stop only the caller's wait: Core may still admit it, and the socket
owner must drain and close any late completion. Socket close or context termination uses the
separate lifecycle cleanup contract.

---

## `zlink_socket_set_receive_flow_state`

Sets this socket's local receive-flow state (`ZLINK_RECEIVE_FLOW_RUNNING` /
`ZLINK_RECEIVE_FLOW_PAUSED`) for a `DEALER` or `ROUTER` socket. Transport lane selection follows
the socket pairing: DEALER-DEALER and DEALER-ROUTER use Application, while ROUTER-ROUTER uses
Completion.

```c
zlink_config_result_t r =
  zlink_socket_set_receive_flow_state(s, ZLINK_RECEIVE_FLOW_PAUSED);
```

**Parameters.** `handle_` is the socket. `state_` is a `zlink_receive_flow_state_t`
(`RUNNING = 0`, `PAUSED = 1`).

**Return and errno.** Returns `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success, including a
repeat of the current state. `ZLINK_CONFIG_INVALID_HANDLE` for a `NULL` or invalid handle.
`ZLINK_CONFIG_INVALID_ARGUMENT` for a state outside `zlink_receive_flow_state_t`.
`ZLINK_CONFIG_NOT_SUPPORTED` for a socket type other than `DEALER`/`ROUTER`; unsupported types
keep their existing byte HWM and transport backpressure unchanged.
`ZLINK_CONFIG_INVALID_STATE` when a concurrent close is admitted first.

**When to use.** `RUNNING`/`PAUSED` is an absolute state, not a counter — repeating the current
state succeeds and resynchronizes nothing new. Completion is the point where the socket-owning
runtime thread stores the local state; it does not mean the remote peer has already observed it.

---

See the [Socket common specification](../spec/core/socket/README.en.md) for the full rationale.
