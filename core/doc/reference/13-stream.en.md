
[Reference index](README.en.md)

# 13. STREAM

A bind-only raw socket that assigns a 4-byte routing ID to every accepted client connection and
exchanges byte records or fixed-framing packets over routed TCP or WebSocket. STREAM does not
support `zlink_connect` and does not interpret application payloads. It reuses
`zlink_socket`/`zlink_bind`/`zlink_close` (Socket lifecycle category), RAW `zlink_recv_part`
(Raw receive category — STREAM is the one type where `source_rid_out_` returns a real view
instead of `NULL`), and completion-backed send through the Socket lifecycle category; this
category covers STREAM's own option, its version of directed send, and the packet-framing
receive mode unique to it. The exact signatures are owned by the
[STREAM specification](../spec/core/socket/08-stream.en.md).

---

## `zlink_set_stream_option` / `zlink_get_stream_option`

Sets or reads STREAM's type-specific receive options.

```c
zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
zlink_set_stream_option(stream_socket, ZLINK_STREAM_OPT_RECV_MODE, &mode, sizeof(mode));
```

**Parameters.** `ZLINK_STREAM_OPT_RECV_MODE` takes RAW or PACKET and must be set before the first
successful bind/connect. `ZLINK_STREAM_OPT_NOTIFY` takes `int` 0 or 1; value `1` exposes client
connect and disconnect as zero-length RAW records and is not supported with PACKET.

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success. Use
`zlink_set_option`/`zlink_get_option` (Socket options and identity category) for common HWM,
timeout, linger, TLS, and buffer options.

**When to use.** Select RAW for unframed bytes and optional connection notifications. Select
PACKET for the fixed header/body framing consumed by `zlink_stream_recv_packet()`.

---

## `zlink_send_part_rid` (STREAM)

Sends one raw data part to a specific connected client by routing ID — STREAM's directed send,
distinct in shape from ROUTER's use of the same function name (ROUTER category).

```c
zlink_send_part_rid(stream_socket, &client_rid, &part, ZLINK_SEND_FLAGS_NONE,
                    ZLINK_PART_FINAL, NULL, NULL);
```

**Parameters.** `target_rid_` must be a valid 4-byte routing ID this STREAM socket assigned.
Every part in a multipart sequence uses the same RID and flags from MORE through FINAL.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on success.
`ZLINK_SUBMIT_NOT_CONNECTED` for a missing connection. Every result consumes `part_`; retain a
separate copy before the first part if the complete record may need application-level recovery.

**When to use.** A NONE FINAL blocks for same-RID local admission within `SNDTIMEO`. DONTWAIT may
return a nonzero completion ID when Core retains the full record; Core then owns same-RID retry
until admission or a terminal result. A failed part atomically discards the staged prefix.

---

## `zlink_stream_recv_packet`

Receives one assembled header/body packet in PACKET mode.

```c
zlink_stream_recv_packet(stream_socket, &source_rid, &header, &body,
                         ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `source_rid_out_` receives the socket-owned client RID view. `header_out_` and
`body_out_` are initialized messages filled on success; each remains a valid message even at
zero length. `flags_` selects blocking NONE or DONTWAIT receive.

**Return and errno.** Returns `zlink_recv_result_t`. No packet under DONTWAIT returns
`ZLINK_RECV_NO_DATA` with `EAGAIN`; RAW mode returns `ZLINK_RECV_NOT_SUPPORTED` with `ENOTSUP`.
On success the caller owns and closes or moves both messages exactly once.

**When to use.** Use this when the application's wire protocol is the fixed frame Core assembles
on each client's byte stream — a 2-byte big-endian `header_size`, a 4-byte big-endian
`body_size`, then exactly `header_size` header bytes followed by exactly `body_size` body bytes.
Choose RAW `zlink_recv_part` instead for byte records without this framing.

---

See the [STREAM specification](../spec/core/socket/08-stream.en.md) for the full rationale.
