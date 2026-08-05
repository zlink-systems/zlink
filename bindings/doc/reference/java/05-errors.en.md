[한국어](05-errors.ko.md) | English

[Reference index](README.en.md)

# 05. Errors

This category is this reference's counterpart to core's result-enum-family table — it documents
the shared exception hierarchy and the seven typed exceptions every submit/request/recv/handler/
close/bind/connect/config-failing API throws (Sockets/Messaging/Eventing/Core categories). The
exact signatures are owned by
[`contracts/errors/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/errors/).

---

## Typed exception family

Each API family throws its own typed exception carrying a typed result enum, rather than one
shared exception type — a caller catches the specific type (or the shared `ZlinkException` base)
and calls `.getResult()`. The hierarchy is a Java `sealed` class: `ZlinkException` (public,
`abstract sealed`, `permits TypedZlinkException`) → `TypedZlinkException` (**package-private**,
`abstract sealed`, not directly referenceable by name from application code) → each concrete
exception below (`public final`).

| Exception | Result enum | Thrown by | Values |
|---|---|---|---|
| `ZlinkSubmitException` | `SubmitResult` (Sockets category) | send/publish/request-submit APIs | `BACKPRESSURED`(1, ordinary control flow), `NOT_CONNECTED`(2), `NOT_FOUND`(3), `TERMINATED`(4), `INVALID_HANDLE`(5), `INVALID_ARGUMENT`(6), `NOT_SUPPORTED`(7), `INVALID_STATE`(8), `THREAD_VIOLATION`(9), `OUT_OF_MEMORY`(10), `SEQ_EXHAUSTED`(11), `INTERNAL_ERROR`(12), `NOT_ADMITTED`(13, ordinary control flow) |
| `ZlinkRequestException` | `RequestResult` (Sockets category) | request/reply completion | `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |
| `ZlinkRecvException` | `RecvResult` (Sockets category) | recv-family APIs | `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206) |
| `ZlinkHandlerException` | `HandlerResult` | handler registration APIs | `INVALID_ARGUMENT`(301), `BUSY`(302), `NOT_SUPPORTED`(303), `DEADLOCK`(304), `INVALID_HANDLE`(305), `INTERNAL_ERROR`(306) |
| `ZlinkCloseException` | `CloseResult` | `close()` paths, `Context.shutdown()` | `BUSY`(401), `SHUTDOWN`(402), `INVALID_HANDLE`(403), `INTERNAL_ERROR`(404) |
| `ZlinkBindException` | `BindResult` | `Socket.bind(...)` | `INVALID_ARGUMENT`(501), `ADDR_IN_USE`(502), `NOT_SUPPORTED`(503), `INVALID_HANDLE`(504), `INTERNAL_ERROR`(505) |
| `ZlinkConnectException` | `ConnectResult` | `connect`/`unbind`/`disconnect`/`disconnectRid` | `INVALID_ARGUMENT`(601), `NOT_SUPPORTED`(602), `INVALID_HANDLE`(603), `INTERNAL_ERROR`(604), `NOT_FOUND`(605), `CONFLICT`(606), `BUSY`(607) |
| `ZlinkConfigException` | `ConfigResult` | every socket/context option getter/setter | `INVALID_HANDLE`(701), `INVALID_ARGUMENT`(702), `NOT_SUPPORTED`(703), `INTERNAL_ERROR`(704), `INVALID_STATE`(705), `NOT_FOUND`(706) |

**Cross-language asymmetry.** `ConfigResult` in this binding has six values, stopping at
`NOT_FOUND`(706) — matching cpp's `config_result_t`, but dotnet's `ZlinkConfigException.ErrorCode`
additionally defines `Conflict`(707), `BufferTooSmall`(708), and `Busy`(709). Whether this
binding's `ConfigResult` should gain those three values is a spec-level question outside this
reference's scope.

**What each value family actually means.** `SubmitResult`'s `BACKPRESSURED`/`NOT_CONNECTED`/
`NOT_FOUND`/`NOT_ADMITTED` are ordinary execution flow, not exceptional failures — a caller that
treats every non-`OK` submit result the same way loses the distinction between "retry is
reasonable" and "this submit will never succeed as constructed." `INVALID_STATE` covers a stale
handle or a closed receive/connection state. Replacing or removing a handler from inside that same
handler's own callback reports `DEADLOCK` rather than actually deadlocking.

---

## `ZlinkException`

The public abstract base every typed exception derives from (through the package-private
intermediate `TypedZlinkException`).

```java
try {
    dealer.send().message(part).submit();
} catch (ZlinkSubmitException ex) {
    if (ex.getResult() == SubmitResult.BACKPRESSURED) {
        // ordinary control flow, not a real failure
    }
}
```

**Options.** Constructors are `protected` — the public entry point for constructing any typed
exception is that exception's own public constructor taking its result enum
(`ZlinkSubmitException(SubmitResult)`, etc.), or the same constructor plus an explicit
`nativeErrno`.

| Member | Meaning |
| --- | --- |
| `getCode()` | `int`, the zlink result code that classifies the failure |
| `getNativeErrno()` | `int`, the underlying native errno, or `0` when none |
| `fromLastError(String operation)` / `fromLastError(ErrorCategory)` | static factory; builds the correctly-typed exception from the current native errno and an `ErrorCategory` (`CONFIG`/`BIND`/`CONNECT`/`CLOSE`/`HANDLER`/`RECV`/`REQUEST`/`SUBMIT`); the `String operation` overload infers the category from the operation name |
| `fromErrno(String operation, int errno)` / `fromErrno(ErrorCategory, int errno)` | static factory; same mapping as `fromLastError` but from an explicit `errno` instead of reading the current native one |

**Completion result.** N/A — this is the exception hierarchy itself. `TypedZlinkException` (the
intermediate sealed class) is package-private — application code can catch/reference
`ZlinkException` or a specific concrete subclass, but cannot name `TypedZlinkException` directly.

**When to use.** Catch the specific typed exception (`ZlinkSubmitException`, etc.) to call its
enum-typed `getResult()`, or catch the shared `ZlinkException` base when only `getCode()`/
`getNativeErrno()` are needed generically across exception types. No-data and transient
back-pressure are never reported as an ordinary exception — see the Sockets/Messaging categories'
`boolean`-returning `recv`/`submit` conventions instead. Use `ZlinkException.fromErrno(...)`/
`fromLastError(...)` only when implementing a custom native interop path that needs to construct a
correctly-typed exception from a raw errno — ordinary application code never needs to call these,
since every built-in API already throws the correctly-typed exception itself.

---

See [`contracts/errors/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/errors/)
and the [Java binding spec](../../spec/java/README.en.md) for the full rationale.
