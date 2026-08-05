[한국어](05-errors.ko.md) | English

[Reference index](README.en.md)

# 05. Errors

This category is this reference's counterpart to core's result-code-family table — it documents
the shared `ZlinkError` interface and the eight typed error structs every submit/request/recv/
handler/close/bind/connect/config-failing API returns. The exact signatures are owned by
[`internal/native/error.go`](../../../../bindings/go/internal/native/error.go) and
[`result_codes.go`](../../../../bindings/go/internal/native/result_codes.go), re-exported as
aliases through [`contracts/errors.go`](../../../../bindings/go/contracts/errors.go).

---

## Typed error family

Each API family has its own typed error struct carrying a typed result code, rather than one shared
error type for every field — a caller type-asserts to the specific struct (or to the shared
`ZlinkError` interface) via `errors.As`. All eight share the identical shape: a `Result` field (its
own named `int` result type), an unexported `nativeErrno int`, and four methods —
`Error() string` (formats as `"<kind> error (<code>): <native strerror text>"` when a native errno
is available, `"<kind> error (<code>)"` otherwise), `Code() int`, `InternalErrno() int`, and
`Unwrap() error` (converts the native errno to a `syscall.Errno`, the standard-library
`errors.Is`/`errors.As` integration point — returns `nil` when no native errno was captured).

| Error struct | Result type | Returned by | Values |
|---|---|---|---|
| `SubmitError` | `SubmitResult` (Messaging category) | send/publish/request-submit APIs | `SubmitOK`(0), `SubmitBackpressured`(1, ordinary control flow), `SubmitNotConnected`(2), `SubmitNotFound`(3), `SubmitTerminated`(4), `SubmitInvalidHandle`(5), `SubmitInvalidArgument`(6), `SubmitNotSupported`(7), `SubmitInvalidState`(8), `SubmitThreadViolation`(9), `SubmitOutOfMemory`(10), `SubmitSeqExhausted`(11), `SubmitInternalError`(12), `SubmitNotAdmitted`(13, ordinary control flow) — the full 13-value set |
| `RequestError` | `RequestResult` | request/reply completion (delivered to the `RequestReplyCallback`/`RequestReplyCompletion`) | `RequestOK`(0), `RequestTimedOut`(101), `RequestNotFound`(102), `RequestTerminated`(103), `RequestProtocolError`(104), `RequestInternalError`(105), `RequestRejected`(106), `RequestConflict`(107), `RequestBusy`(108), `RequestNotConnected`(109), `RequestInvalidArgument`(110), `RequestInvalidState`(111), `RequestNotSupported`(112), `RequestBackpressured`(113) — **this binding does define `Backpressured`**, unlike rust's `RequestResult`, which has no equivalent |
| `RecvError` | `RecvResult` | recv-family APIs | `RecvOK`(0), `RecvNoData`(201), `RecvBusy`(202), `RecvTerminated`(203), `RecvInvalidHandle`(204), `RecvNotSupported`(205), `RecvInternalError`(206), `RecvBufferTooSmall`(207), `RecvInvalidState`(208) — the full 8-value set, matching node's, not the 6-value set dotnet/cpp/java/rust/python share |
| `HandlerError` | `HandlerResult` | handler registration APIs (`OnEvent`/`OnFire`/`OnPacket`/`OnCompletionControl`, etc.) | `HandlerOK`(0), `HandlerInvalidArgument`(301), `HandlerBusy`(302), `HandlerNotSupported`(303), `HandlerDeadlock`(304), `HandlerInvalidHandle`(305), `HandlerInternalError`(306) |
| `CloseError` | `CloseResult` | `Close()` paths, `Context.Shutdown()` | `CloseOK`(0), `CloseBusy`(401), `CloseShutdown`(402), `CloseInvalidHandle`(403), `CloseInternalError`(404) |
| `BindError` | `BindResult` | `Bind(...)` | `BindOK`(0), `BindInvalidArgument`(501), `BindAddrInUse`(502), `BindNotSupported`(503), `BindInvalidHandle`(504), `BindInternalError`(505) |
| `ConnectError` | `ConnectResult` | `Connect`/`Unbind`/`Disconnect`/`DisconnectRID` | `ConnectOK`(0), `ConnectInvalidArgument`(601), `ConnectNotSupported`(602), `ConnectInvalidHandle`(603), `ConnectInternalError`(604), `ConnectNotFound`(605), `ConnectConflict`(606), `ConnectBusy`(607), `ConnectAuthFailed`(608) — the full 8-value set including `AuthFailed`, matching node's, not the 7-value set dotnet/cpp/java/rust/python share |
| `ConfigError` | `ConfigResult` | every socket/context option getter/setter | `ConfigOK`(0), `ConfigInvalidHandle`(701), `ConfigInvalidArgument`(702), `ConfigNotSupported`(703), `ConfigInternalError`(704), `ConfigInvalidState`(705), `ConfigNotFound`(706), `ConfigConflict`(707), `ConfigBufferTooSmall`(708), `ConfigBusy`(709) — the full 9-value set, matching dotnet's/node's, not the 6-value set cpp/java share |

**Cross-language asymmetry, restated here.** Every wrapper binding's result set is supposed to
mirror core's `zlink_*_result_t` families exactly (documented in core's Errors category), but they
don't all agree with each other. This binding's `RecvResult` and `ConnectResult` carry the fuller
value sets that only node was previously documented as having — **go joins node** in defining
`BufferTooSmall`/`InvalidState` on `RecvResult` and `AuthFailed` on `ConnectResult`, so any earlier
claim that node is "the only one" with those values is now stale and should be read as "node and
go." This binding's `RequestResult` is complete (has `Backpressured`), unlike rust's, which lacks
it. Whether the languages missing values should gain them, or whether the fuller ones should be
pared back, is a spec-level question outside this reference's scope — this entry states the fact
so it doesn't get lost.

**Completion result.** Every error is returned as a plain Go `error` interface value holding one of
these eight concrete pointer types; a caller recovers the typed value with `errors.As(err,
&target)`.

**When to use.** Match on `.Result` for a specific error family when the calling code needs to
branch on the result code; use `errors.Is`/`errors.As` against the `Unwrap()`-exposed
`syscall.Errno` when integrating with code that already handles POSIX errno values generically.

---

## `ZlinkError`

The shared error interface every one of the eight typed error structs implements — for code that
wants to handle any zlink-originated error uniformly without knowing which specific struct it is.

```go
var zerr contracts.ZlinkError
if errors.As(err, &zerr) {
    log.Printf("zlink error %d (errno %d): %v", zerr.Code(), zerr.InternalErrno(), zerr)
}
```

**Options.** `ZlinkError` is a Go `interface` (not a struct or enum). **`Unwrap() error` is not
part of the `ZlinkError` interface itself** — every concrete struct implements it, but a caller
holding only a `ZlinkError`-typed value cannot call `Unwrap()` directly through that interface;
`errors.Is`/`errors.As` still find it via reflection on the concrete value underneath, per the
standard library's own convention.

| Member | Meaning |
| --- | --- |
| `error` | embeds the standard error interface, i.e. requires `Error() string` |
| `Code() int` | the zlink result code that classifies the failure |
| `InternalErrno() int` | the underlying native errno |

**Completion result.** N/A — a pure interface type, never constructed directly.

**When to use.** Use `errors.As(err, &zerr)` (where `zerr` is declared as `contracts.ZlinkError`)
when a call site wants `.Code()`/`.InternalErrno()` without caring which of the eight concrete
error structs produced it. Use the specific struct type (e.g. `*contracts.RecvError`) instead when
the result code's meaning is family-specific.

---

## No `Strerror` function

Unlike every language that exposes a standalone errno-to-text lookup, **this binding has no public
`Strerror`/`strerror` function at all** (noted already in the Core category, restated here since
it belongs to this category's subject matter). The native `zlink_strerror` call is used internally
only, inside each typed error's `Error()` method — every error formats its own message directly
rather than routing through a shared exported lookup a caller could call independently.

**When to use.** Call `.Error()` on the typed error (or let `fmt`/`log` do so implicitly) to get the
formatted message; there is no other public path to the native strerror text in this binding.

---

See
[`internal/native/error.go`](../../../../bindings/go/internal/native/error.go),
[`result_codes.go`](../../../../bindings/go/internal/native/result_codes.go), and the
[Go binding spec](../../spec/go/README.en.md) for the full rationale.
