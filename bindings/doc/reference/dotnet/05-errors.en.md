[한국어](05-errors.ko.md) | English

[Reference index](README.en.md)

# 05. Errors

This category is this reference's counterpart to core's result-enum-family table — it documents
the shared exception base and the seven typed exceptions every submit/request/recv/handler/close/
bind/connect/config-failing API throws (Sockets/Messaging/Eventing/Core categories). The exact
signatures are owned by
[`Contracts/Errors/`](../../../../bindings/dotnet/src/Zlink/Contracts/Errors/).

---

## Typed exception family

Each API family throws its own typed exception carrying a nested `ErrorCode` enum, rather than one
shared exception type — a caller catches the specific type (or the shared `ZlinkException` base)
and branches on `.Result`.

| Exception | Thrown by | `ErrorCode` values |
|---|---|---|
| `ZlinkSubmitException` | send/publish/request-submit APIs (every socket-type category) | `Backpressured`(1, ordinary control flow), `NotConnected`(2), `NotFound`(3), `Terminated`(4), `InvalidHandle`(5), `InvalidArgument`(6), `NotSupported`(7), `InvalidState`(8), `ThreadViolation`(9), `OutOfMemory`(10), `SeqExhausted`(11), `InternalError`(12), `NotAdmitted`(13, ordinary control flow — the target was reachable but an admission policy rejected it) |
| `ZlinkRequestException` | request/reply completion | `TimedOut`(101), `NotFound`(102), `Terminated`(103), `ProtocolError`(104), `InternalError`(105), `Rejected`(106), `Conflict`(107), `Busy`(108), `NotConnected`(109), `InvalidArgument`(110), `InvalidState`(111), `NotSupported`(112) |
| `ZlinkRecvException` | recv-family APIs (Sockets/Eventing categories) | `NoData`(201), `Busy`(202), `Terminated`(203), `InvalidHandle`(204), `NotSupported`(205), `InternalError`(206) |
| `ZlinkHandlerException` | handler registration APIs (Sockets/Eventing categories) | `InvalidArgument`(301), `Busy`(302), `NotSupported`(303), `Deadlock`(304, replacing/removing a handler from inside its own callback), `InvalidHandle`(305), `InternalError`(306) |
| `ZlinkCloseException` | `Close()`/`Dispose` paths (Sockets/Eventing categories), `IContext.Shutdown()` (Core category) | `Busy`(401), `Shutdown`(402), `InvalidHandle`(403), `InternalError`(404) |
| `ZlinkBindException` | `ISocket.Bind(...)` (Sockets category) | `InvalidArgument`(501), `AddrInUse`(502), `NotSupported`(503), `InvalidHandle`(504), `InternalError`(505) |
| `ZlinkConnectException` | `IConnectableSocket.Connect`/`Unbind`/`Disconnect`/`DisconnectRid` (Sockets category) | `InvalidArgument`(601), `NotSupported`(602), `InvalidHandle`(603), `InternalError`(604), `NotFound`(605), `Conflict`(606), `Busy`(607) |
| `ZlinkConfigException` | every socket/context option getter/setter (Sockets/Core categories) | `InvalidHandle`(701), `InvalidArgument`(702), `NotSupported`(703), `InternalError`(704), `InvalidState`(705), `NotFound`(706), `Conflict`(707), `BufferTooSmall`(708), `Busy`(709) |

**What each value family actually means.** `ZlinkSubmitException`'s `Backpressured`/
`NotConnected`/`NotFound`/`NotAdmitted` are ordinary execution flow, not exceptional failures — a
caller that treats every non-`Ok` submit result the same way loses the distinction between "retry
is reasonable" and "this submit will never succeed as constructed." `ZlinkConfigException`'s
`BufferTooSmall` means the caller-provided output capacity couldn't hold the first complete value;
the call consumes nothing, so retrying with a larger buffer is safe. `InvalidState` covers a stale
handle or a closed receive/connection state. Replacing or removing a handler from inside that same
handler's own callback reports `Deadlock` rather than actually deadlocking.

---

## `ZlinkException`

The abstract base every typed exception above derives from.

```csharp
try
{
    dealer.Send().Message(Message.From("payload")).Submit();
}
catch (ZlinkSubmitException ex) when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
{
    // ordinary control flow, not a real failure
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `ZlinkException(int code)` / `ZlinkException(int code, int nativeErrno)` | protected constructors only — the public entry point is each typed exception's own constructor taking its `ErrorCode` (never `Ok`, see below) |
| `Code` | `int`, the zlink result code that classifies the failure |
| `NativeErrno` | `int`, the underlying native errno, `0` when none |

**Completion result.** N/A — this is the exception hierarchy itself. Every typed exception's public
constructor rejects the success value `Ok` via `ValidatePublicErrorCode<TErrorCode>`, throwing
`ArgumentOutOfRangeException` if a caller tries to construct one with `Ok`. A constructor overload
that also accepts a native errno exists only for internal runtime conversion and is not public
surface.

**When to use.** Catch the specific typed exception (`ZlinkSubmitException`, etc.) to branch on its
`ErrorCode`-typed `.Result`, or catch the shared `ZlinkException` base when only `.Code`/
`.NativeErrno` are needed generically across exception types. No-data and transient back-pressure
are never reported as an ordinary exception — see the Sockets/Messaging categories' `bool`-returning
`Recv`/`Submit` conventions instead.

---

## `SubmitResult`

A public enum with the same values as `ZlinkSubmitException.ErrorCode`, used internally to map a
native result code before it becomes a typed exception or a `bool` return.

**Options.** Same value set as `ZlinkSubmitException.ErrorCode` (above).

**Completion result.** N/A — no public API in `Contracts/` returns or accepts `SubmitResult`
directly; every public submit surface (Messaging/Sockets categories) reports failure as `bool` or
`ZlinkSubmitException`, not this enum.

**When to use.** Not applicable from application code today — it exists as a public type but isn't
reached through any public contract member. Whether it should be merged into
`ZlinkSubmitException.ErrorCode` or made genuinely reachable is a spec-level question outside this
reference's scope.

---

See [`Contracts/Errors/`](../../../../bindings/dotnet/src/Zlink/Contracts/Errors/) and the
[.NET binding spec](../../spec/dotnet/README.en.md) for the full rationale.
