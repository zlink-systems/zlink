[한국어](13-stream.ko.md) | English

[Reference index](README.en.md)

# 13. STREAM

A bind-only raw socket that assigns a 4-byte routing ID to every accepted client connection and
exchanges byte records or fixed-framing packets over routed TCP or WebSocket. STREAM does not
support `zlink_connect` and does not interpret application payloads. It reuses
`zlink_socket`/`zlink_bind`/`zlink_close` (Socket lifecycle category), `zlink_recv_part`/
`zlink_recv_handler` (Raw receive category — STREAM is the one type where `source_rid_out_`
returns a real view instead of `NULL`), and `zlink_send_ready_handler` (Socket lifecycle
category) unchanged; this category covers STREAM's own option, its version of directed send, and
the packet-framing receive mode unique to it. The exact signatures are owned by the
[STREAM specification](../spec/core/socket/08-stream.en.md).

---

## `zlink_set_stream_option` / `zlink_get_stream_option`

Sets or reads STREAM's one type-specific option.

```c
int notify = 1;
zlink_set_stream_option(stream_socket, ZLINK_STREAM_OPT_NOTIFY, &notify, sizeof(notify));
```

**Parameters.** `option_` is `ZLINK_STREAM_OPT_NOTIFY` (`int` 0 or 1) — must be set before
`zlink_bind`. `1` exposes client connect and disconnect as zero-length data records, with the
source routing ID identifying the client.

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success. Use
`zlink_set_option`/`zlink_get_option` (Socket options and identity category) for common HWM,
timeout, linger, TLS, and buffer options.

**When to use.** Enable `ZLINK_STREAM_OPT_NOTIFY` when the application needs to detect client
connect/disconnect through the same receive path as data, without a separate monitor.

---

## `zlink_send_part_rid` (STREAM)

Sends one raw data part to a specific connected client by routing ID — STREAM's directed send,
distinct in shape from ROUTER's use of the same function name (ROUTER category).

```c
zlink_send_part_rid(stream_socket, &client_rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `target_rid_` must be a valid 4-byte routing ID this STREAM socket assigned.
`part_flag_` must be `ZLINK_PART_FINAL` — passing `ZLINK_PART_MORE` returns
`ZLINK_SUBMIT_NOT_SUPPORTED` with `ENOTSUP`, since STREAM send never opens a multipart sequence.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on success.
`ZLINK_SUBMIT_NOT_CONNECTED` for a missing connection. `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN`
on backpressure — unlike other socket types, this specific failure leaves `part_` owned by the
caller and retryable as-is; every other failure (and success) still consumes it.

**When to use.** Because STREAM send is always single-part, the atomic multipart-abort rule that
applies to PAIR/DEALER/ROUTER/PUB does not apply here — a `ZLINK_PART_MORE` failure stages
nothing, and the next call is an independent record. Keeping a payload copy before the call still
gives one uniform recovery strategy across every failure result, including the one case
(backpressure) where the original content survives.

---

## `zlink_stream_packet_handler`

Attaches a packet-framed receive callback — one of the three mutually exclusive STREAM receive
modes (with `zlink_recv_part` and `zlink_recv_handler`, Raw receive category).

```c
zlink_stream_packet_handler(stream_socket, on_packet, userdata);
```

**Parameters.** `handler_` is a `zlink_stream_packet_handler_fn`, invoked per assembled packet
with the source routing ID and separate `header_`/`body_` messages. Both are delivered as valid
`zlink_msg_t` values even at zero length (never `NULL`), and ownership of both transfers to the
callback — each must be consumed or closed exactly once.

**Return and errno.** Returns `zlink_handler_result_t` — `ZLINK_HANDLER_OK` on success. The first
raw part receive or handler registration on a handle fixes its receive mode; activating a
different mode, or registering this handler again, fails with a busy result and `EBUSY`.

**When to use.** Use this when the application's wire protocol is the fixed frame Core assembles
on each client's byte stream — a 2-byte big-endian `header_size`, a 4-byte big-endian
`body_size`, then exactly `header_size` header bytes followed by exactly `body_size` body bytes.
Choose `zlink_recv_part` instead for raw byte records with no framing, or `zlink_recv_handler`
for a raw push callback without this frame assembly.

---

See the [STREAM specification](../spec/core/socket/08-stream.en.md) for the full rationale.
