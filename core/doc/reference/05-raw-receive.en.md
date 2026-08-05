[한국어](05-raw-receive.ko.md) | English

[Reference index](README.en.md)

# 05. Raw receive

This category covers the two receive entry points shared across socket types: part-based receive
and the raw callback. Which of these — or a socket-type-specific receive family — applies to a
given socket type is fixed, not a runtime choice; see the table below and each socket type's own
category. The exact signatures are owned by the
[Socket common specification](../spec/core/socket/README.en.md).

---

## `zlink_recv_part`

Receives one message part from a raw socket, synchronously.

```c
zlink_msg_t part;
zlink_msg_init(&part);

const zlink_routing_id_t *source_rid;
zlink_part_flag_t has_more;
zlink_recv_result_t result = zlink_recv_part(s, &source_rid, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `source_rid_out_` is optional and receives a Core-owned view (copy it if it must
outlive the next raw recv call on the same thread; `STREAM` returns a real view, `PAIR` and
`DEALER` return `NULL`). `part_out_` must point to an already-initialized message and is
required. `has_more_out_` is required and is set to `ZLINK_PART_MORE` or `ZLINK_PART_FINAL`.
`flags_` is `ZLINK_RECV_FLAGS_NONE` (blocking) or `ZLINK_RECV_FLAGS_DONTWAIT`.

**Return and errno.** Returns `zlink_recv_result_t` — `ZLINK_RECV_OK` on success, with ownership
of the received part transferred to the caller (close it exactly once with
`zlink_msg_close`/`zlink_multipart_close`, Message category). `ZLINK_RECV_NOT_SUPPORTED` with
`ENOTSUP` for an unsupported socket type. With `ZLINK_RECV_FLAGS_DONTWAIT`, no available part
returns `ZLINK_RECV_NO_DATA` with `EAGAIN`. A failure never transfers part ownership.

**When to use.** Supported types are raw `PAIR`, `DEALER`, and `STREAM`. `PUB`, `XPUB`, `SUB`,
`XSUB`, and `ROUTER` are not supported here — use their dedicated receive entries instead (SUB
and XSUB categories' `zlink_subscribe_part`, ROUTER category's `zlink_router_recv_part`). Receive
every part of one multipart message, first through last, with this function on the same thread.
Pair this with a poller observing `ZLINK_POLLIN` (Polling and pollers category) for the primary
`recv + poller` model.

---

## `zlink_recv_handler`

Attaches a raw receive callback to a `STREAM` socket, replacing the `recv + poller` model with
push delivery.

```c
zlink_recv_handler(stream_socket, on_raw_message, userdata);
```

**Parameters.** `handler_` is a `zlink_socket_msg_handler_fn` — invoked on the owning I/O thread
with the source routing ID, an array of message parts, and the part count; ownership of every
part transfers to the callback, and each must be closed exactly once. `userdata_` is passed
through.

**Return and errno.** Returns `zlink_handler_result_t` — `ZLINK_HANDLER_OK` on success.
`ENOTSUP` for any subject other than raw `STREAM`. After a successful attach, `zlink_recv_part`,
`zlink_stream_packet_handler` (STREAM category), and data-plane poller `ZLINK_POLLIN` on the same
handle fail with `EBUSY` — a second attach on the same handle also fails with `EBUSY`.

**When to use.** Choose this, `zlink_recv_part`, or `zlink_stream_packet_handler` (STREAM
category) — exactly one raw receive mode per `STREAM` handle; see the receive-surface table
below and the STREAM category for the packet-framed alternative.

---

## Receive surface by socket type

| Socket type | Receive surface | Notes |
|---|---|---|
| PAIR | `zlink_recv_part()` | part receive only |
| DEALER | `zlink_recv_part()` (+ `zlink_dealer_request_part()` completion callback) | part-receive data plane |
| SUB / XSUB | `zlink_subscribe_part()` | topic-part receive only — see SUB/XSUB categories |
| ROUTER | `zlink_router_recv_part()` (+ `zlink_router_request_part()` completion callback) | part-receive data plane — see ROUTER category |
| STREAM | `zlink_recv_part()` / `zlink_recv_handler()` / `zlink_stream_packet_handler()` | exception: choose exactly one mode — see STREAM category |
| PUB | N/A | send-only |
| XPUB | `zlink_xpub_recv_part()` (subscription events, recv-only) | data plane is send — see XPUB category |
| monitor / timer | recv and callback both supported | see Socket monitor and Timers categories |

The request-completion callback on `DEALER`/`ROUTER` is an async operation-completion surface,
not a data-plane receive callback — the two roles stay separate even though both use a callback.
`STREAM` is the one exception where an application picks among three receive models per handle; a
second attempt to activate a different mode on the same handle fails with `EBUSY`.

---

See the [Socket common specification](../spec/core/socket/README.en.md) for the full rationale.
