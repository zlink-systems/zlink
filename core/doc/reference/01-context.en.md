[한국어](01-context.ko.md) | English

[Reference index](README.en.md)

# 01. Context

This category covers the entry points a `zlink_ctx_*` handle provides: creation, shutdown,
option configuration, and the automatic-HWM recalculation trigger. The exact signatures are
owned by the [Context specification](../spec/core/01-context.en.md).

---

## `zlink_ctx_new`

Creates a new context. The prerequisite for every other entry in this reference — every socket
belongs to a context.

```c
void *ctx = zlink_ctx_new();
```

**Parameters.** None.

**Return and errno.** Returns a context handle on success, or `NULL` on failure with `errno`
set. Option values start at their defaults (`ZLINK_IO_THREADS_DFLT` = 4,
`ZLINK_MAX_SOCKETS_DFLT` = 4095, etc. — see the Context specification's default-values table).

**When to use.** Call this once per context your process needs. A context manages a pool of I/O
threads and may be shared across threads; most applications need exactly one.

---

## `zlink_ctx_shutdown` / `zlink_ctx_term`

Signals in-flight blocking calls to unblock, then destroys the context and releases its
resources.

```c
zlink_ctx_shutdown(ctx);   // non-blocking: unblocks pending calls with ETERM
zlink_ctx_term(ctx);       // blocks until every socket in the context is closed
```

**Parameters.** Both take only the context handle.

**Return and errno.** Both return `zlink_close_result_t` — `ZLINK_CLOSE_OK` on success. `term`
fails with `EFAULT` (invalid handle) or `EINTR` (interrupted by a signal; retry it). `shutdown`
fails only with `EFAULT`. After `term` returns, the handle must not be used again.

**When to use.** Call `shutdown` first when sockets are in use across multiple threads, to avoid
a thread blocking on a socket call forever — it makes every blocking call on the context's
sockets return `ETERM` immediately. Call `term` exactly once per context, always, to release
resources; it may block until every socket the context owns has been closed.

---

## `zlink_ctx_set` / `zlink_ctx_get`

Sets or reads a context option whose public type is `int`.

```c
zlink_ctx_set(ctx, ZLINK_IO_THREADS, 8);

zlink_config_result_t err;
int threads = zlink_ctx_get(ctx, ZLINK_IO_THREADS, &err);
```

**Parameters.** `option_` is one of the `zlink_ctx_option_t` values (`ZLINK_IO_THREADS`,
`ZLINK_MAX_SOCKETS`, `ZLINK_THREAD_PRIORITY`, `ZLINK_THREAD_SCHED_POLICY`, `ZLINK_MAX_MSGSZ`,
`ZLINK_THREAD_AFFINITY_CPU_ADD`/`_REMOVE`, `ZLINK_CTX_OPT_BLOCKY`,
`ZLINK_CTX_OPT_AUTO_HWM_ENABLE`, `ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS`,
`ZLINK_CTX_OPT_AUTO_HWM_PROFILE`; see the Context specification's option table for each one's
meaning and default). `zlink_ctx_get` additionally writes read-only options such as
`ZLINK_SOCKET_LIMIT` and `ZLINK_MSG_T_SIZE`.

**Return and errno.** `zlink_ctx_set` returns `zlink_config_result_t` — `ZLINK_CONFIG_OK` on
success, `EINVAL` for an unknown option or out-of-range value, `EFAULT`
(`ZLINK_CONFIG_INVALID_HANDLE`) for an invalid context. `zlink_ctx_get` returns the option value
directly on success, or `-1` with the `zlink_config_result_t` written through `error_out_` on
failure.

**When to use.** Use these for the `int`-typed options above. `ZLINK_CTX_OPT_AUTO_HWM_PROFILE`
and `ZLINK_CTX_OPT_AUTO_HWM_ENABLE` are safe to change on a live context — the profile change
applies to the next automatic HWM recalculation, and the enable toggle applies immediately to
sockets still on automatic HWM. `ZLINK_SOCKET_LIMIT` and `ZLINK_THREAD_PRIORITY` share enum
value `3`; the lookup resolves to the read-only `ZLINK_SOCKET_LIMIT`, so `ZLINK_THREAD_PRIORITY`
cannot actually be set or read through this pair.

---

## `zlink_ctx_set_data` / `zlink_ctx_get_data`

Sets or reads a context option whose public type is not a plain `int` — a byte buffer or string.

```c
uint64_t unit_bytes = 2048;
zlink_ctx_set_data(ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_bytes, sizeof(unit_bytes));

const char *prefix = "app-io";
zlink_ctx_set_data(ctx, ZLINK_THREAD_NAME_PREFIX, prefix, strlen(prefix) + 1);
```

**Parameters.** `option_` is `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` (requires exactly
`sizeof(uint64_t)` bytes; `0` selects the socket type's default unit) or
`ZLINK_THREAD_NAME_PREFIX` (a null-terminated string, `optvallen_` including the terminator,
bounded to 16 bytes for the platform thread-name limit).

**Return and errno.** Both return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success,
`EINVAL` for an unknown option, an invalid value, or (for the HWM unit option) any size other
than exactly `sizeof(uint64_t)` — including a legacy 4-byte value, which is rejected rather than
reinterpreted. `EFAULT` (`ZLINK_CONFIG_INVALID_HANDLE`) for an invalid context.

**When to use.** Use these only for the two options above; every other option goes through
`zlink_ctx_set`/`zlink_ctx_get`. `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` is a planning input for
the automatic HWM planner, not an observed average message size.

---

## `zlink_ctx_auto_hwm_recalculate`

Forces an immediate automatic-HWM refresh for every socket in the context still on automatic
policy.

```c
zlink_ctx_auto_hwm_recalculate(ctx);
```

**Parameters.** Only the context handle.

**Return and errno.** Returns `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success, `EFAULT`
for an invalid context handle.

**When to use.** Call this after changing the automatic HWM profile or a message-unit option, to
apply the new per-connection sizing immediately instead of waiting for the normal refresh path.
Sockets with a manual HWM override, or with automatic HWM disabled, are unaffected.

---

See the [Context specification](../spec/core/01-context.en.md) for the full rationale.
