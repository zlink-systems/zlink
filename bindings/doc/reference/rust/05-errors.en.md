[한국어](05-errors.ko.md) | English

[Reference index](README.en.md)

# 05. Errors

This page covers the shared error enum and the typed error structs that the submit, request, recv,
handler, close, bind, connect, and config APIs return via `Result::Err`. The exact signatures are in
[`contracts/errors/`](../../../../bindings/rust/src/contracts/errors/).

---

## Typed error family

Each API family uses a typed error struct that holds its result enum. The caller can match on the
struct's `.code` field or the shared `ZlinkError` enum. The `define_error_type!` macro defines a
`pub code: TResult` field, a `code()` accessor, and a `pub const fn internal_errno(&self) -> i32`
accessor on each struct. Each struct implements `Display`, `std::error::Error`, and
`From<Self> for ZlinkError`.

The struct retains its `pub native_errno: i32` field until the next breaking release. The constructor is `pub const fn new(code: TResult, native_errno: i32) -> Self`. `TResult` denotes the struct's result enum type.

On each struct and on `ZlinkError`, `native_errno()` is a `#[deprecated]` alias that delegates to `internal_errno()` and returns the same value. It will be removed in the next breaking release.

| Error struct | Result enum | Returned by | Values |
|---|---|---|---|
| `SubmitError` | `SubmitResult` (Messaging category) | send/publish/request-submit APIs | `Backpressured`(1, ordinary control flow), `NotConnected`(2), `NotFound`(3), `Terminated`(4), `InvalidHandle`(5), `InvalidArgument`(6), `NotSupported`(7), `InvalidState`(8), `ThreadViolation`(9), `OutOfMemory`(10), `SeqExhausted`(11), `InternalError`(12), `NotAdmitted`(13, ordinary control flow) |
| `RequestError` | `RequestResult` | Future `submit()` or blocking `submit_sync()` terminal request failure | `TimedOut`(101), `NotFound`(102), `Terminated`(103), `ProtocolError`(104), `InternalError`(105), `Rejected`(106), `Conflict`(107), `Busy`(108), `NotConnected`(109), `InvalidArgument`(110), `InvalidState`(111), `NotSupported`(112), `Backpressured`(113) |
| `RecvError` | `RecvResult` | recv-family APIs | `NoData`(201), `Busy`(202), `Terminated`(203), `InvalidHandle`(204), `NotSupported`(205), `InternalError`(206) — the 6-value set (no `BufferTooSmall`/`InvalidState`, matching dotnet/cpp/java, not node's 8-value set) |
| `HandlerError` | `HandlerResult` | retained result family; current public completion/event delivery does not register handlers | `InvalidArgument`(301), `Busy`(302), `NotSupported`(303), `Deadlock`(304), `InvalidHandle`(305), `InternalError`(306) |
| `CloseError` | `CloseResult` | `close()` paths, `Context::shutdown()` | `Busy`(401), `Shutdown`(402), `InvalidHandle`(403), `InternalError`(404) |
| `BindError` | `BindResult` | `bind(...)` | `InvalidArgument`(501), `AddrInUse`(502), `NotSupported`(503), `InvalidHandle`(504), `InternalError`(505) |
| `ConnectError` | `ConnectResult` | `connect`/`unbind`/`disconnect`/`disconnect_rid` | `InvalidArgument`(601), `NotSupported`(602), `InvalidHandle`(603), `InternalError`(604), `NotFound`(605), `Conflict`(606), `Busy`(607) — 7 values (no `AuthFailed`, matching dotnet/cpp/java, not node's 8-value set) |
| `ConfigError` | `ConfigResult` | every socket/context option getter/setter | `InvalidHandle`(701), `InvalidArgument`(702), `NotSupported`(703), `InternalError`(704), `InvalidState`(705), `NotFound`(706) — the 6-value set (matching cpp/java, not dotnet's/node's 9-value set) |

**Cross-language asymmetry, restated here.** Every wrapper binding's result enum is supposed to
mirror core's `zlink_*_result_t` families exactly (documented in core's Errors category), but they
do not all expose the same auxiliary receive/connect/config values. `RequestResult`, however,
includes the common `Backpressured` terminal at 113.

**What each value family actually means.** `SubmitResult`'s `Backpressured`/`NotConnected`/
`NotFound`/`NotAdmitted` are ordinary execution flow, not exceptional failures — code that treats
every non-`Ok` submit result the same way loses the distinction between "retry is reasonable" and
"this submit will never succeed as constructed." `InvalidState` covers a stale handle or a closed
receive/connection state. `HandlerResult` remains part of the result model, but current public
send/request terminals and pull-event surfaces do not produce it.

---

## `ZlinkError`

The Rust-idiomatic shared error type: an `enum` with one variant per typed error struct, rather
than an inheritance base class.

```rust
match dealer.send().message(part).submit_sync() {
    Ok(()) => { /* Core reported terminal acceptance */ }
    Err(err) => {
        let zlink_err: ZlinkError = err.into();
        if zlink_err.code() == SubmitResult::Backpressured as i32 {
            // ordinary control flow, not a real failure
        }
    }
}
```

**Options.** Each typed error struct implements `From<Self> for ZlinkError`, so `.into()`/`?` (with
an error type of `ZlinkError`) converts automatically.

| Member | Meaning |
| --- | --- |
| `Submit(SubmitError)` / `Request(RequestError)` / `Recv(RecvError)` / `Handler(HandlerError)` / `Close(CloseError)` / `Bind(BindError)` / `Connect(ConnectError)` / `Config(ConfigError)` | one variant per typed error struct |
| `code(&self) -> i32` | matches on the variant, returns the inner `code` field cast to `i32` |
| `pub fn internal_errno(&self) -> i32` | returns the errno from the error struct held by the variant, or `0` when none is available |

**Completion result.** N/A — a plain sum-type wrapper implementing `Display` and
`std::error::Error`.

**When to use.** Match on the specific typed error struct returned by a given API when the specific
result enum's variants matter; convert into `ZlinkError` (`.into()`, or let `?` do it via a
function returning `Result<_, ZlinkError>`) when a single error type across every API in a function
body is more convenient than matching on eight distinct struct types. No-data and transient
back-pressure are never reported as an error — see the Sockets/Messaging categories' `bool`/
`Option`-returning `recv`/`submit` conventions instead.

---

See [`contracts/errors/`](../../../../bindings/rust/src/contracts/errors/) and the
[Rust binding spec](../../spec/rust/README.en.md) for the full rationale.
