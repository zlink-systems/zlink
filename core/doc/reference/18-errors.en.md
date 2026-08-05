[한국어](18-errors.ko.md) | English

[Reference index](README.en.md)

# 18. Errors, results, and version

This category is this reference's counterpart to framework's error-kind correspondence table: it
explains the shared failure model every other category's "Return and errno" section relies on,
and covers the three small version/error-introspection entry points. Public functions return
their primary control flow through a `zlink_*_result_t` enum and record a more detailed cause in
`zlink_errno()` for the same thread — callers branch on the result enum and use errno for
logging and finer diagnosis. Success is always numeric zero, and errno is unspecified after
success. The exact signatures are owned by the
[Errors specification](../spec/core/03-errors.en.md) and the
[errno map](../spec/core/04-errno-map.en.md).

---

## Result enum families

Each API family returns its own typed result enum rather than a single shared error kind. This
table is the index every other category's entries point back to.

| Enum | Used by | Common values |
|---|---|---|
| `zlink_submit_result_t` | send/publish/request-submit APIs (every socket-type category) | `OK`(0), `BACKPRESSURED`(1, normal control flow), `NOT_CONNECTED`(2), `NOT_FOUND`(3), `NOT_ADMITTED`(13, normal control flow — target identified but admission policy rejects), `TERMINATED`(4), `INVALID_HANDLE`(5), `INVALID_ARGUMENT`(6), `NOT_SUPPORTED`(7), `INVALID_STATE`(8), `THREAD_VIOLATION`(9), `OUT_OF_MEMORY`(10), `SEQ_EXHAUSTED`(11), `INTERNAL_ERROR`(12) |
| `zlink_request_result_t` | `zlink_reply_handler_fn` completion (DEALER/ROUTER categories) | `OK`(0), `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |
| `zlink_recv_result_t` | recv-family APIs (Raw receive, Socket monitor, Timers categories) | `OK`(0), `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206), `BUFFER_TOO_SMALL`(207), `INVALID_STATE`(208) |
| `zlink_handler_result_t` | handler-registration APIs (Raw receive, Socket lifecycle, Socket monitor, Timers categories) | `OK`(0), `INVALID_ARGUMENT`(301), `BUSY`(302), `NOT_SUPPORTED`(303), `DEADLOCK`(304), `INVALID_HANDLE`(305), `INTERNAL_ERROR`(306) |
| `zlink_close_result_t` | `zlink_ctx_term`/`zlink_close`/`zlink_ctx_shutdown`/`zlink_timer_destroy`/`zlink_monitor_close` | `OK`(0), `BUSY`(401), `SHUTDOWN`(402), `INVALID_HANDLE`(403), `INTERNAL_ERROR`(404) |
| `zlink_bind_result_t` | `zlink_bind` (Socket lifecycle category) | `OK`(0), `INVALID_ARGUMENT`(501), `ADDR_IN_USE`(502), `NOT_SUPPORTED`(503), `INVALID_HANDLE`(504), `INTERNAL_ERROR`(505) |
| `zlink_connect_result_t` | `zlink_connect`/`zlink_unbind`/`zlink_disconnect`/`zlink_disconnect_rid` (Socket lifecycle category) | `OK`(0), `INVALID_ARGUMENT`(601), `NOT_SUPPORTED`(602), `INVALID_HANDLE`(603), `INTERNAL_ERROR`(604), `NOT_FOUND`(605), `CONFLICT`(606), `BUSY`(607), `AUTH_FAILED`(608) |
| `zlink_config_result_t` | every `zlink_set_*`/`zlink_get_*` option API, and misc. control-path calls | `OK`(0), `INVALID_HANDLE`(701), `INVALID_ARGUMENT`(702), `NOT_SUPPORTED`(703), `INTERNAL_ERROR`(704), `INVALID_STATE`(705), `NOT_FOUND`(706), `CONFLICT`(707), `BUFFER_TOO_SMALL`(708), `BUSY`(709) |

**What each value family means in practice** (see the [errno map](../spec/core/04-errno-map.en.md)
for the exact per-function mapping to `errno`): `BACKPRESSURED`/`NOT_CONNECTED`/`NOT_FOUND`/
`NOT_ADMITTED` on submit are normal runtime control flow, not exceptional failures — a caller
that treats every non-`OK` submit result the same way loses the distinction between "retry is
reasonable" and "this will never succeed as submitted." `BUFFER_TOO_SMALL` on receive/config
means a caller-provided buffer can't hold the first complete value (or, for SUB/XSUB, that the
topic buffer capacity is too small) — the call reports the required size without consuming
anything, so a retry with a bigger buffer is safe. `INVALID_STATE` covers a stale handle or
closed receive/connection state. Unregistering or replacing a handler from inside that same
handler's callback returns `DEADLOCK` rather than deadlocking for real.

---

## `zlink_errno` / `zlink_strerror`

Reads the calling thread's detailed error code, or converts an error code to a human-readable
string.

```c
int code = zlink_errno();
const char *message = zlink_strerror(code);
```

**Parameters.** `zlink_errno` takes no arguments. `zlink_strerror` takes `errnum` — an error
code, typically from a prior `zlink_errno()` call.

**Return and errno.** `zlink_errno` returns the calling thread's current errno value — call it
immediately after a failing operation returns, before any other Core call on the same thread.
`zlink_strerror` returns a pointer to library-owned static storage the caller must not free or
modify.

**When to use.** Use these together for diagnostics/logging once a `zlink_*_result_t` value has
already told you *which* result-enum bucket a failure fell into — `errno` gives platform/detail
context on top of that, not a substitute classification. POSIX errno values missing on a given
platform use public values based on `ZLINK_HAUSNUMERO`; `ESTALE`/`EALREADY`/`EDEADLK`/
`ESHUTDOWN` are guaranteed available on every supported platform for stale handles, duplicate
operations, reentrant callbacks, and closed sockets respectively.

---

## `zlink_version`

Reads the library's build version.

```c
int major, minor, patch;
zlink_version(&major, &minor, &patch);
```

**Parameters.** Three output `int *` pointers.

**Return and errno.** None (`void`). `ZLINK_VERSION_MAJOR`/`_MINOR`/`_PATCH` and the combined
`ZLINK_VERSION`/`ZLINK_MAKE_VERSION(major, minor, patch)` macros are also available at compile
time for version checks that don't need a runtime call. Core uses SOVERSION matching
`ZLINK_VERSION_MAJOR`.

**When to use.** Use this at runtime to confirm the linked library version matches what the
application was built against, especially when Core is loaded dynamically rather than linked at
build time.

---

See the [Errors specification](../spec/core/03-errors.en.md) and the
[errno map](../spec/core/04-errno-map.en.md) for the full rationale.
