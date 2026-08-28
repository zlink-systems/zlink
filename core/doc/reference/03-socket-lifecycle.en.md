
[Reference index](README.en.md)

# 03. Socket lifecycle

This category covers the entry points common to every raw socket type: creation, endpoint
lifecycle (bind/connect/disconnect), closing, asynchronous send, and receive flow control.
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
`zlink_router_recv_part()`, and only `STREAM` offers a choice among raw part receive, a raw
callback, or a packet callback (see the STREAM category). The socket must be closed with
`zlink_close` before the context terminates.

---

## `zlink_close`

Closes a socket and releases its resources.

```c
zlink_close_result_t result = zlink_close(s);
```

**Parameters.** Only the socket handle.

**Return and errno.** Returns `zlink_close_result_t` — `ZLINK_CLOSE_OK` on success. `ENOTSOCK`
if the handle is not a valid socket, `EBUSY` if a callback or operation is in-flight on the
handle from another thread.

**When to use.** Call this to release a socket you no longer need. Outstanding send-queue
messages are discarded or sent depending on `ZLINK_OPT_LINGER` (Socket options and identity
category). Public handles follow a tiered thread-safety contract — hot-path `send` may be called
concurrently, control-path calls (bind/connect/option/monitor) serialize for correctness, and
`close` uses a stricter fail-fast gate: once accepted, new API entry on the same handle fails
with `ESHUTDOWN`. Self-close from inside a send-completion or monitor callback is deferred until
the callback returns rather than failing.

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

## `zlink_send_async` / `zlink_send_complete_handler` / `zlink_send_async_cancel`

Hands one complete multipart record to Core for asynchronous send, installs or replaces the
callback that reports how a pending send resolves, and requests cancellation of one
not-yet-completed operation.

```c
zlink_send_complete_handler(s, on_send_complete, userdata);

zlink_send_op_id_t op_id = 0;
zlink_send_async_options_t opts = { .struct_size = sizeof(opts), .timeout_ms = 1000 };
zlink_submit_result_t r = zlink_send_async(s, parts, part_count, &opts, &op_id);
if (r == ZLINK_SUBMIT_OK && op_id != 0) {
    // op_id is non-zero only while completion is still pending.
    zlink_send_async_cancel(s, op_id);
}
```

**Parameters.** `zlink_send_async`'s `parts_`/`part_count_` is the multipart record; `options_`
(`zlink_send_async_options_t`, `struct_size` set to `sizeof`) carries a per-operation
`timeout_ms` (`0` = no deadline, unrelated to `ZLINK_OPT_SNDTIMEO`), an opaque `userdata` echoed
back in the completion event, and an optional `target` (`zlink_routed_submit_target_t`) for a
previously snapshotted routed pipe. `op_id_out_` may be `NULL` when the caller does not need to
distinguish immediate admission from pending completion. `zlink_send_complete_handler`'s
`handler_` is a `zlink_send_complete_handler_fn`; passing `NULL` is invalid — the call replaces,
it does not unregister. `zlink_send_async_cancel` takes the `op_id` returned by a prior
`zlink_send_async` call.

**Return and errno.** `zlink_send_async` returns `zlink_submit_result_t` —
`ZLINK_SUBMIT_OK` with `*op_id_out_ == 0` means immediate admission and no completion callback
runs; `ZLINK_SUBMIT_OK` with a non-zero id means the record is pending and will receive exactly
one completion. `ZLINK_SUBMIT_BACKPRESSURED` when a configured
`ZLINK_OPT_SEND_PENDING_MAX_MSGS`/`_BYTES` bound is exceeded (part ownership stays with the
caller). `EINVAL` if no completion handler was installed first (a handler is required because
the call can become pending) or the subject is unsupported by `zlink_send_async`. `ENOTSUP` for
a socket type outside raw `PAIR`/`DEALER`/`ROUTER`/`STREAM`. `zlink_send_complete_handler`
returns `zlink_handler_result_t` — `ZLINK_HANDLER_OK` on success, `ENOTSUP` for a socket type
outside that same set, `EDEADLK` if called reentrantly from inside a completion callback on the
same handle. `zlink_send_async_cancel` returns `zlink_submit_result_t` —
`ZLINK_SUBMIT_OK` means the cancel was accepted and the completion will report
`ZLINK_SEND_TERMINAL`/`ECANCELED`; `ZLINK_SUBMIT_NOT_FOUND` when no pending operation carries
that id; `ZLINK_SUBMIT_INVALID_STATE` when another resolver already claimed the operation (that
resolver still reports exactly one completion).

**When to use.** On `ZLINK_SUBMIT_OK`, ownership of every entry in `parts_[0 .. part_count_)`
transfers to Core; the caller must not touch those messages again, close included, on any other
result ownership stays with the caller. `ZLINK_SEND_ADMITTED` in the completion event
(`zlink_send_complete_event_t`, carrying `op_id`, `userdata`, `peer_rid`, the transport-pair
identity, `result`, and `terminal_errno`) means admission into the Core send queue, not peer
delivery — use request/reply when delivery confirmation is required. Completions for one socket
never run concurrently with each other, but no fixed thread is promised: a callback can run on
the Core async mailbox thread, the Core deadline thread on timeout, the closing thread during
close, or — once the socket is registered on a poller with `ZLINK_POLLCOMPLETION` — on the
thread that calls `zlink_poller_wait` (Polling and pollers category), which is a change of
dispatch location only. The callback must only hand the completion to application state; calling
any send, publish, or request entry point on any socket from inside it fails with `EDEADLK`, as
does replacing a send-completion handler on any socket. A cancelled operation still completes
exactly once.

---

## `zlink_socket_set_receive_flow_state`

Sets this socket's local receive-flow state (`ZLINK_RECEIVE_FLOW_RUNNING` /
`ZLINK_RECEIVE_FLOW_PAUSED`) and synchronizes it to the paired `DEALER`/`ROUTER` completion lane.

```c
zlink_config_result_t r =
  zlink_socket_set_receive_flow_state(s, ZLINK_RECEIVE_FLOW_PAUSED);
```

**Parameters.** `handle_` is the socket. `state_` is a `zlink_receive_flow_state_t`
(`RUNNING = 0`, `PAUSED = 1`).

**Return and errno.** Returns `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success, including a
repeat of the current state. `ZLINK_CONFIG_INVALID_HANDLE` for a `NULL` or invalid handle.
`ZLINK_CONFIG_INVALID_ARGUMENT` for a state outside `zlink_receive_flow_state_t`.
`ZLINK_CONFIG_NOT_SUPPORTED` for a socket type other than `DEALER`/`ROUTER`, which has no
completion lane and keeps its existing byte HWM and transport backpressure unchanged.
`ZLINK_CONFIG_INVALID_STATE` when a concurrent close is admitted first.

**When to use.** `RUNNING`/`PAUSED` is an absolute state, not a counter — repeating the current
state succeeds and resynchronizes nothing new. Completion is the point where the socket-owning
runtime thread stores the local state; it does not mean the remote peer has already observed it.

---

See the [Socket common specification](../spec/core/socket/README.en.md) for the full rationale.
