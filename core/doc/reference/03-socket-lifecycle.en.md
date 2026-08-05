[한국어](03-socket-lifecycle.ko.md) | English

[Reference index](README.en.md)

# 03. Socket lifecycle

This category covers the entry points common to every raw socket type: creation, endpoint
lifecycle (bind/connect/disconnect), closing, and the send-ready callback. Per-socket-type
options and data-plane operations live in their own categories (PAIR/PUB/SUB/XPUB/XSUB/DEALER/
ROUTER/STREAM below). The exact signatures are owned by the
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
with `ESHUTDOWN`. Self-close from inside a send-ready or monitor callback is deferred until the
callback returns rather than failing.

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

## `zlink_send_ready_handler`

Installs or replaces a callback invoked when a send-capable socket leaves backpressure and a
retry is worth attempting.

```c
zlink_send_ready_handler(s, on_send_ready, userdata);
```

**Parameters.** `handler_` is a `zlink_send_ready_handler_fn`; passing `NULL` is invalid — this
call replaces, it does not unregister. `userdata_` is passed through to the callback.

**Return and errno.** Returns `zlink_handler_result_t` — `ZLINK_HANDLER_OK` on success.
`ENOTSUP` for a socket type that doesn't support it. `EDEADLK` if called reentrantly from the
same handle's send-ready callback.

**When to use.** Supported on raw `PAIR`, `PUB`, `XPUB`, `DEALER`, `ROUTER`, and `STREAM`. This
callback shares the same send-recovery readiness axis as `ZLINK_POLLOUT` (Polling and pollers
category) — after a `BACKPRESSURED` result from a send, the signal tells the caller retrying is
worthwhile, not that the retry will succeed; the very next retry may still return
`BACKPRESSURED`. A successful replace becomes visible starting from the next writable
transition.

---

See the [Socket common specification](../spec/core/socket/README.en.md) for the full rationale.
