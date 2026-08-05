[한국어](05-errors.ko.md) | English

[Reference index](README.en.md)

# 05. Errors

This category is this reference's counterpart to core's result-enum-family table — it documents
the shared error base and the seven typed errors every submit/request/recv/handler/close/bind/
connect/config-failing API throws (Sockets/Messaging/Eventing/Core categories). The exact
signatures are owned by
[`contracts/errors/`](../../../../bindings/node/src/zlink/contracts/errors/).

---

## Typed error family

Each API family throws its own typed error carrying a typed result constant, rather than one
shared error type — a caller catches the specific type (or the shared `ZlinkError` base) and reads
`.result`. Every typed error extends an internal, **not exported** `ResultError<TResult>` class
(analogous to java's package-private `TypedZlinkException`), which itself extends the public
`ZlinkError`.

| Error | Result constant | Thrown by | Values |
|---|---|---|---|
| `SubmitError` | `SubmitResult` (Sockets category) | send/publish/request-submit APIs | `Backpressured`(1, ordinary control flow), `NotConnected`(2), `NotFound`(3), `Terminated`(4), `InvalidHandle`(5), `InvalidArgument`(6), `NotSupported`(7), `InvalidState`(8), `ThreadViolation`(9), `OutOfMemory`(10), `SeqExhausted`(11), `InternalError`(12), `NotAdmitted`(13, ordinary control flow) |
| `RequestError` | `RequestResult` | request/reply completion | `TimedOut`(101), `NotFound`(102), `Terminated`(103), `ProtocolError`(104), `InternalError`(105), `Rejected`(106), `Conflict`(107), `Busy`(108), `NotConnected`(109), `InvalidArgument`(110), `InvalidState`(111), `NotSupported`(112), `Backpressured`(113) |
| `RecvError` | `RecvResult` | recv-family APIs | `NoData`(201), `Busy`(202), `Terminated`(203), `InvalidHandle`(204), `NotSupported`(205), `InternalError`(206), `BufferTooSmall`(207), `InvalidState`(208) — **this binding's `RecvResult` includes `BufferTooSmall`/`InvalidState`, matching go's; every other language shares a 6-value set without them** |
| `HandlerError` | `HandlerResult` | handler registration APIs | `InvalidArgument`(301), `Busy`(302), `NotSupported`(303), `Deadlock`(304), `InvalidHandle`(305), `InternalError`(306) |
| `CloseError` | `CloseResult` | `close()` paths, `Context.shutdown()` | `Busy`(401), `Shutdown`(402), `InvalidHandle`(403), `InternalError`(404) |
| `BindError` | `BindResult` | `Socket.bind(...)` | `InvalidArgument`(501), `AddrInUse`(502), `NotSupported`(503), `InvalidHandle`(504), `InternalError`(505) |
| `ConnectError` | `ConnectResult` | `connect`/`unbind`/`disconnect`/`disconnectRid` | `InvalidArgument`(601), `NotSupported`(602), `InvalidHandle`(603), `InternalError`(604), `NotFound`(605), `Conflict`(606), `Busy`(607), `AuthFailed`(608) — **this binding has `AuthFailed`, matching go's; every other language shares a 7-value set without it** |
| `ConfigError` | `ConfigResult` | every socket/context option getter/setter | `InvalidHandle`(701), `InvalidArgument`(702), `NotSupported`(703), `InternalError`(704), `InvalidState`(705), `NotFound`(706), `Conflict`(707), `BufferTooSmall`(708), `Busy`(709) — the full 9-value set (matching dotnet, unlike cpp/java's 6-value set) |

**Cross-language asymmetry, corrected here.** Every wrapper binding's result constant set is
supposed to mirror core's `zlink_*_result_t` families exactly (documented in core's Errors
category), but they don't all agree with each other: dotnet's `RecvResult`/`ConnectResult` are
missing `BufferTooSmall`/`InvalidState` and `AuthFailed` respectively; cpp's and java's
`ConfigResult` stop at `NotFound`(706), missing `Conflict`/`BufferTooSmall`/`Busy`. Node's three
result sets documented here match core's full definitions — **go's do too**, so this is a
two-binding pairing, not a node-only trait. Whether the other bindings should gain these missing
values, or whether node/go should be pared back to match them, is a spec-level question outside
this reference's scope — this entry states the fact so it doesn't get lost.

**What each value family actually means.** `SubmitResult`'s `Backpressured`/`NotConnected`/
`NotFound`/`NotAdmitted` are ordinary execution flow, not exceptional failures — a caller that
treats every non-`Ok` submit result the same way loses the distinction between "retry is
reasonable" and "this submit will never succeed as constructed." `RecvResult`/`ConfigResult`'s
`BufferTooSmall` means the caller-provided output capacity couldn't hold the first complete value;
the call consumes nothing, so retrying with a larger buffer is safe. `InvalidState` covers a stale
handle or a closed receive/connection state. Replacing or removing a handler from inside that same
handler's own callback reports `Deadlock` rather than actually deadlocking.

---

## `ZlinkError`

The public base every typed error derives from (through the non-exported intermediate
`ResultError<TResult>`).

```ts
try {
  dealer.send().message(part).submit();
} catch (err) {
  if (err instanceof SubmitError && err.result === SubmitResult.Backpressured) {
    // ordinary control flow, not a real failure
  }
}
```

**Options.** Public constructor `ZlinkError(code: number, nativeErrno = 0)` — but application code
never constructs this directly; every built-in API throws the correctly-typed subclass instead.

| Member | Meaning |
| --- | --- |
| `code` | `number`, the zlink result code that classifies the failure |
| `nativeErrno` | `number`, the underlying native errno, defaults to `0` |
| `name` | `string`, set to the concrete error class name (e.g. `'SubmitError'`) by `ResultError`'s constructor |

**Completion result.** N/A — this is the error hierarchy itself, extending the built-in `Error`.
The internal `ResultError<TResult>` layer is not exported — application code can catch/reference
`ZlinkError` or a specific concrete subclass (`SubmitError`, etc.), but cannot import
`ResultError` directly.

**When to use.** Catch the specific typed error (`SubmitError`, etc.) and read its typed `.result`,
or catch the shared `ZlinkError` (or plain `Error`) when only `.code`/`.nativeErrno` are needed
generically across error types. No-data and transient back-pressure are never reported as an
ordinary thrown error — see the Sockets/Messaging categories' `boolean`/`null`-returning
`recv`/`submit` conventions instead.

---

See [`contracts/errors/`](../../../../bindings/node/src/zlink/contracts/errors/) and the
[Node binding spec](../../spec/node/README.en.md) for the full rationale.
