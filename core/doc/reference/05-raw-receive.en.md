
[Reference index](README.en.md)

# 05. Raw receive

This category covers part-based DATA receive shared across raw socket types. A socket-type-specific
receive family may apply instead; see the table below and each socket type's own category. STREAM
also makes an explicit RAW/PACKET choice before its first bind or connect. The exact signatures are owned by the
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
outlive the next data-receive entry on the same socket; `STREAM` returns a real view, `PAIR` and
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

## STREAM RAW and PACKET receive

A STREAM socket selects one pull receive family before its first successful bind or connect.

```c
zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
zlink_set_stream_option(stream_socket, ZLINK_STREAM_OPT_RECV_MODE,
                        &mode, sizeof(mode));
```

**Parameters.** `ZLINK_STREAM_RECV_MODE_RAW` selects `zlink_recv_part()`;
`ZLINK_STREAM_RECV_MODE_PACKET` selects `zlink_stream_recv_packet()`. PACKET receive fills
caller-initialized `header_out_` and `body_out_` messages and returns the source routing-id view.

**Return and errno.** Bind or connect without a selected mode fails with `EINVAL`. After the first
successful bind or connect, changing even to the current mode fails with `EBUSY`. Calling the
receive family for the other mode returns `ZLINK_RECV_NOT_SUPPORTED` with `ENOTSUP`.

**When to use.** Choose RAW for unframed byte records and pair it with `ZLINK_POLLIN`. Choose
PACKET when the wire protocol uses Core's fixed header/body framing; see the STREAM category.

---

## Receive surface by socket type

| Socket type | Receive surface | Notes |
|---|---|---|
| PAIR | `zlink_recv_part()` | part receive only |
| DEALER | `zlink_recv_part()` + `zlink_completion_recv()` | DATA uses part receive; submitted request results use completion receive |
| SUB / XSUB | `zlink_subscribe_part()` | topic-part receive only — see SUB/XSUB categories |
| ROUTER | `zlink_router_recv_part()` + `zlink_completion_recv()` | DATA/REQUEST uses part receive; submitted request results use completion receive |
| STREAM | `zlink_recv_part()` or `zlink_stream_recv_packet()` | choose RAW or PACKET before bind/connect — see STREAM category |
| PUB | N/A | send-only |
| XPUB | `zlink_xpub_recv_part()` (subscription events, recv-only) | data plane is send — see XPUB category |
| monitor / timer | pull receive | see Socket monitor and Timers categories |

The REQUEST completion queue on `DEALER`/`ROUTER` is an operation-completion surface, not DATA
receive. STREAM selects exactly one of two pull receive modes before endpoint activation.

---

See the [Socket common specification](../spec/core/socket/README.en.md) for the full rationale.
