[한국어](05-errors.ko.md) | English

[Reference index](README.en.md)

# 05. Errors

This category is this reference's counterpart to core's result-enum-family table — it documents
the shared exception base and the seven typed exceptions every submit/request/recv/handler/close/
bind/connect/config-failing API throws (Sockets/Messaging/Eventing/Core categories). The exact
signatures are owned by
[`Contracts/Errors/`](../../../../bindings/cpp/include/zlink/Contracts/Errors/).

---

## Typed exception family

Each API family throws its own typed exception (deriving from `binding_error_t`) carrying its own
result enum, rather than one shared exception type — a caller catches the specific type (or the
shared `binding_error_t` base) and calls `.result()`.

| Exception | Result enum | Thrown by | Values |
|---|---|---|---|
| `submit_error_t` | `submit_result_t` (Sockets category) | send/publish/request-submit APIs | `backpressured`(1, ordinary control flow), `not_connected`(2), `not_found`(3), `terminated`(4), `invalid_handle`(5), `invalid_argument`(6), `not_supported`(7), `invalid_state`(8), `thread_violation`(9), `out_of_memory`(10), `seq_exhausted`(11), `internal_error`(12), `not_admitted`(13, ordinary control flow) |
| `request_error_t` | `request_result_t` (Messaging category) | request/reply completion | `timed_out`(101), `not_found`(102), `terminated`(103), `protocol_error`(104), `internal_error`(105), `rejected`(106), `conflict`(107), `busy`(108), `not_connected`(109), `invalid_argument`(110), `invalid_state`(111), `not_supported`(112) |
| `recv_error_t` | `recv_result_t` (Sockets category) | recv-family APIs | `no_data`(201), `busy`(202), `terminated`(203), `invalid_handle`(204), `not_supported`(205), `internal_error`(206) |
| `handler_error_t` | `handler_result_t` | handler registration APIs | `invalid_argument`(301), `busy`(302), `not_supported`(303), `deadlock`(304), `invalid_handle`(305), `internal_error`(306) |
| `close_error_t` | `close_result_t` | `close()` paths, `context_t::shutdown()` | `busy`(401), `shutdown`(402), `invalid_handle`(403), `internal_error`(404) |
| `bind_error_t` | `bind_result_t` | `socket_t::bind(...)` | `invalid_argument`(501), `addr_in_use`(502), `not_supported`(503), `invalid_handle`(504), `internal_error`(505) |
| `connect_error_t` | `connect_result_t` | `connect`/`unbind`/`disconnect`/`disconnect_rid` | `invalid_argument`(601), `not_supported`(602), `invalid_handle`(603), `internal_error`(604), `not_found`(605), `conflict`(606), `busy`(607) |
| `config_error_t` | `config_result_t` | every socket/context option getter/setter | `invalid_handle`(701), `invalid_argument`(702), `not_supported`(703), `internal_error`(704), `invalid_state`(705), `not_found`(706) |

**Cross-language asymmetry.** `config_result_t` in this projection has six values, stopping at
`not_found`(706) — dotnet's `ZlinkConfigException.ErrorCode` additionally defines `Conflict`(707),
`BufferTooSmall`(708), and `Busy`(709). Whether this projection's `config_result_t` should gain
those three values is a spec-level question outside this reference's scope, not something this
document resolves.

**What each value family actually means.** `submit_error_t`'s `backpressured`/`not_connected`/
`not_found`/`not_admitted` are ordinary execution flow, not exceptional failures — a caller that
treats every non-zero submit result the same way loses the distinction between "retry is
reasonable" and "this submit will never succeed as constructed." `invalid_state` covers a stale
handle or a closed receive/connection state. Replacing or removing a handler from inside that same
handler's own callback reports `deadlock` rather than actually deadlocking.

---

## `binding_error_t`

The abstract base every typed exception above derives from (itself derived from
`std::runtime_error`).

```cpp
try {
    std::move (dealer.send ()).message (part).submit ();
} catch (const zlink::submit_error_t &ex) {
    if (ex.result () == zlink::submit_result_t::backpressured) {
        // ordinary control flow, not a real failure
    }
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `binding_error_t(int code_, int internal_errno_)` | protected constructor only — the public entry point is each typed exception's own constructor taking its result enum (e.g. `submit_error_t(submit_result_t)`), or the same plus an explicit `internal_errno_` used internally when converting from a native result |
| `code()` | `int`, the zlink result code that classifies the failure |
| `internal_errno()` | `int`, the underlying native errno, or the same value as `code()` when constructed with the one-argument form |
| `what()` | overridden `std::runtime_error::what()`, returns the formatted message text |

**Completion result.** N/A — this is the exception hierarchy itself. `error_t` (the general,
non-family-specific exception) can be constructed directly from a raw code via `error_t(int code_)`
or `error_t(int code_, int internal_errno_)`.

**When to use.** Catch the specific typed exception (`submit_error_t`, etc.) to call its
enum-typed `.result()`, or catch the shared `binding_error_t` base (or plain `std::exception`) when
only `.code()`/`.what()` are needed generically across exception types. No-data and transient
back-pressure are never reported as an ordinary exception — see the Sockets/Messaging categories'
`int`/`bool`-returning `recv`/`submit` conventions instead.

---

See [`Contracts/Errors/`](../../../../bindings/cpp/include/zlink/Contracts/Errors/) and the
[C++ binding spec](../../spec/cpp/README.en.md) for the full rationale.
