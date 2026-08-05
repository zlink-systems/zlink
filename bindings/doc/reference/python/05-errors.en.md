[한국어](05-errors.ko.md) | English

[Reference index](README.en.md)

# 05. Errors

This category is this reference's counterpart to core's result-enum-family table — it documents
the shared exception base and the seven typed exceptions every submit/request/recv/handler/close/
bind/connect/config-failing API raises (Sockets/Messaging/Eventing/Core categories). The exact
signatures are owned by
[`contracts/errors/`](../../../../bindings/python/src/zlink/contracts/errors/).

---

## Typed exception family

Each API family raises its own typed exception carrying a typed result enum, rather than one
shared exception type — a caller catches the specific type (or the shared `ZlinkError` base) and
reads `.result`. All eight are concrete classes (not `Protocol`s, unlike most of this binding's
other contracts) deriving from an internal `_TypedZlinkError` base, itself deriving from
`ZlinkError(RuntimeError)`.

| Exception | Result enum | Raised by | Values |
|---|---|---|---|
| `SubmitError` | `SubmitResult` (Sockets category) | send/publish/request-submit APIs | `BACKPRESSURED`(1, ordinary control flow), `NOT_CONNECTED`(2), `NOT_FOUND`(3), `TERMINATED`(4), `INVALID_HANDLE`(5), `INVALID_ARGUMENT`(6), `NOT_SUPPORTED`(7), `INVALID_STATE`(8), `THREAD_VIOLATION`(9), `OUT_OF_MEMORY`(10), `SEQ_EXHAUSTED`(11), `INTERNAL_ERROR`(12), `NOT_ADMITTED`(13, ordinary control flow) |
| `RequestError` | `RequestResult` (Sockets category) | request/reply completion | `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |
| `RecvError` | `RecvResult` (Sockets category) | recv-family APIs | `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206), `BUFFER_TOO_SMALL`(207), `INVALID_STATE`(208) — the fuller 8-value set (matching node's) |
| `HandlerError` | `HandlerResult` (Sockets category) | handler registration APIs | `INVALID_ARGUMENT`(301), `BUSY`(302), `NOT_SUPPORTED`(303), `DEADLOCK`(304), `INVALID_HANDLE`(305), `INTERNAL_ERROR`(306) |
| `CloseError` | `CloseResult` | `close()` paths, `Context.shutdown()` | `BUSY`(401), `SHUTDOWN`(402), `INVALID_HANDLE`(403), `INTERNAL_ERROR`(404) |
| `BindError` | `BindResult` | `Socket.bind(...)` | `INVALID_ARGUMENT`(501), `ADDR_IN_USE`(502), `NOT_SUPPORTED`(503), `INVALID_HANDLE`(504), `INTERNAL_ERROR`(505) |
| `ConnectError` | `ConnectResult` | `connect`/`disconnect`/`disconnect_rid` | `INVALID_ARGUMENT`(601), `NOT_SUPPORTED`(602), `INVALID_HANDLE`(603), `INTERNAL_ERROR`(604), `NOT_FOUND`(605), `CONFLICT`(606), `BUSY`(607), `AUTH_FAILED`(608) — the fuller 8-value set (matching node's) |
| `ConfigError` | `ConfigResult` | every socket/context option getter/setter | `INVALID_HANDLE`(701), `INVALID_ARGUMENT`(702), `NOT_SUPPORTED`(703), `INTERNAL_ERROR`(704), `INVALID_STATE`(705), `NOT_FOUND`(706), `CONFLICT`(707), `BUFFER_TOO_SMALL`(708), `BUSY`(709) — the full 9-value set (matching dotnet's/node's) |

**Cross-language asymmetry, restated here.** This binding's `RecvResult`/`ConnectResult`/
`ConfigResult` match node's fuller value sets rather than the smaller ones dotnet/cpp/java/rust use
in one or more of these families — the same asymmetry already documented in every other language's
Errors category. Whether the smaller sets should gain the missing values, or this binding's fuller
sets should be pared back, is a spec-level question outside this reference's scope.

**What each value family actually means.** `SubmitResult`'s `BACKPRESSURED`/`NOT_CONNECTED`/
`NOT_FOUND`/`NOT_ADMITTED` are ordinary execution flow, not exceptional failures — code that treats
every non-`OK` submit result the same way loses the distinction between "retry is reasonable" and
"this submit will never succeed as constructed." `BUFFER_TOO_SMALL` means the caller-provided
output capacity couldn't hold the first complete value; the call consumes nothing, so retrying with
a larger buffer is safe. `INVALID_STATE` covers a stale handle or a closed receive/connection
state. Replacing or removing a handler from inside that same handler's own callback reports
`DEADLOCK` rather than actually deadlocking.

---

## `ZlinkError`

The public base every typed exception derives from (through the internal `_TypedZlinkError`
intermediate).

```python
try:
    dealer.send().message(part).submit()
except SubmitError as ex:
    if ex.result == SubmitResult.BACKPRESSURED:
        pass  # ordinary control flow, not a real failure
```

**Options.**

| Member | Meaning |
| --- | --- |
| `ZlinkError(code: int, native_errno: int = 0)` | a plain public constructor (unlike languages where the base is protected/abstract and only reachable via a subclass) |
| `code` | property, the zlink result code that classifies the failure |
| `native_errno` | property, the underlying native errno, `0` when none |
| `_TypedZlinkError.result` | property, the typed result enum; declared on the non-public intermediate every typed exception actually derives from — its `__init__` **catches a `ValueError` when the raw code doesn't match any known enum member and preserves the raw integer instead of raising or silently mapping to an unrelated value**, specifically to tolerate a newer Core version reporting a result this binding doesn't know about yet |

**Completion result.** N/A — this is the exception hierarchy itself, extending the built-in
`RuntimeError`.

**When to use.** Catch the specific typed exception (`SubmitError`, etc.) to read its typed
`.result`, or catch the shared `ZlinkError` when only `.code`/`.native_errno` are needed generically
across exception types. No-data and transient back-pressure are never reported as an ordinary
exception — see the Sockets/Messaging categories' `bool`/`None`-returning `recv_into`/`submit`
conventions instead. Because `.result` can hold a raw `int` instead of the enum when talking to a
newer Core, compare it with `==` against the enum member rather than assuming `isinstance(ex.result,
SubmitResult)` always holds.

---

See [`contracts/errors/`](../../../../bindings/python/src/zlink/contracts/errors/) and the
[Python binding spec](../../spec/python/README.en.md) for the full rationale.
