[한국어](04-socket-options.ko.md) | English

[Reference index](README.en.md)

# 04. Socket options and identity

This category covers the entry points that configure a socket after creation: the common option
pair, routing identity, and TLS role configuration. Socket-type-specific options (e.g.
`zlink_set_router_option`) live in their own socket-type category instead of here. The exact
signatures are owned by the [Socket common specification](../spec/core/socket/README.en.md).

---

## `zlink_set_option` / `zlink_get_option`

Sets or reads a common socket option shared across every socket type and discovery.

```c
uint64_t sndhwm = 8_000_000;
zlink_set_option(s, ZLINK_OPT_SNDHWM, &sndhwm, sizeof(sndhwm));

int linger = -1;
size_t linger_len = sizeof(linger);
zlink_get_option(s, ZLINK_OPT_LINGER, &linger, &linger_len);
```

**Parameters.** `handle_` may be a raw socket or discovery. `option_` is a `zlink_option_t` value
(transport/buffer: `AFFINITY`, `RATE`, `RECOVERY_IVL`, `SNDBUF`/`RCVBUF`, `SNDHWM`/`RCVHWM`,
`AUTO_HWM_MSG_UNIT_BYTES`, `MAXMSGSIZE`; timing: `LINGER`, `RCVTIMEO`/`SNDTIMEO`,
`CONNECT_TIMEOUT`, `RECONNECT_IVL`/`_MAX`, `HANDSHAKE_IVL`, `SUBMIT_RETRY_*`; TCP:
`TCP_KEEPALIVE*`, `TCP_MAXRT`, `TCP_NODELAY`; network: `IPV6`, `TOS`, `MULTICAST_*`,
`BINDTODEVICE`, `BACKLOG`; TLS: `TLS_CERT`/`_KEY`/`_CA`/`_VERIFY`/`_REQUIRE_CLIENT_CERT`/
`_HOSTNAME`/`_TRUST_SYSTEM`/`_PASSWORD`; behavior: `IMMEDIATE`, `CONFLATE`, `BLOCKY` (read via
`ZLINK_CTX_OPT_BLOCKY` instead — see the Socket specification's constants table for the full
list and defaults), `INVERT_MATCHING`, `ZMP_METADATA`; read-only: `FD`, `EVENTS`, `TYPE`,
`LAST_ENDPOINT`, `ROUTE_VALUE_MAX_SIZE`). `optval_`/`optvallen_` supply or receive the value and
its byte size — `SNDHWM`, `RCVHWM`, and `AUTO_HWM_MSG_UNIT_BYTES` require exactly
`sizeof(uint64_t)` bytes.

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
`EINVAL` for an unknown option, an out-of-range value, or a byte-count option not using the
exact required size (a legacy 4-byte value is rejected, not reinterpreted). `ETERM` if the
context was terminated (`set` only).

**When to use.** Use this pair for every option except routing ID, TLS role configuration, and
subscribe/unsubscribe, which have dedicated functions below and in the SUB/XSUB category. HWM is
applied per directional pipe in accounted bytes: once the limit is reached, further writes wait
for byte credit from the receiver, though an empty pipe may still admit one oversized message
(bounded by `MAXMSGSIZE`) before blocking. Core normally batches credit at `ceil(hwm_bytes / 2)`
and may return one early credit update before the low-water mark if the sender is currently
HWM-blocked and the receiver has drained all visible input.

---

## `zlink_set_routing_id` / `zlink_get_routing_id`

Sets or reads the routing identity a socket presents to peers.

```c
zlink_set_routing_id(s, "worker-3", 8);

zlink_routing_id_t rid;
zlink_get_routing_id(s, &rid);
```

**Parameters.** `set` takes `data_`/`size_` — 1..255 binary-safe bytes; must be called before
`bind`/`connect`. `get` writes into a caller-owned `zlink_routing_id_t *out_`.

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
`ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP` for a handle kind that doesn't support routing
identity.

**When to use.** Set this before connecting or binding if the peer needs a stable, predictable
identity; otherwise Core assigns a 16-byte RFC 4122 UUID v4 as raw bytes (not a UUID string) when
the socket is created. `ZLINK_OPT_RID_DUPLICATE_POLICY` (via `zlink_set_option`) controls what
happens if a local socket observes another peer advertising the same routing ID
(`ZLINK_RID_DUPLICATE_REJECT` keeps the existing pipe; `ZLINK_RID_DUPLICATE_HANDOVER` lets a
same-direction reconnect take over) — this option has no effect on `STREAM`, which assigns its
own 4-byte connection routing IDs.

---

## `zlink_set_tls_server` / `zlink_set_tls_client`

Configures TLS server or client role on a socket that supports it.

```c
zlink_set_tls_server(s, "server.crt", "server.key", /*require_client_cert=*/0);
// or, on the connecting side:
zlink_set_tls_client(s, "ca-bundle.crt", "server.example.com", /*trust_system=*/1);
```

**Parameters.** `set_tls_server` takes `cert_`/`key_` (PEM file paths) and
`require_client_cert_` (1 to require mutual authentication). `set_tls_client` takes `ca_cert_`
(PEM CA bundle path), `hostname_` (expected hostname for SNI and certificate verification), and
`trust_system_` (1 to also trust the system CA store).

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
`ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP` for an unsupported raw socket type or handle kind.

**When to use.** Use `set_tls_server` on the binding side and `set_tls_client` on the connecting
side of a `tls://` endpoint (Socket lifecycle category's `zlink_bind`/`zlink_connect`). These are
the standard role-configuration entry points; the individual `ZLINK_OPT_TLS_*` values
(`zlink_set_option`/`zlink_get_option` above) configure or query individual TLS values only on
raw network sockets that support TLS.

---

See the [Socket common specification](../spec/core/socket/README.en.md) for the full rationale.
