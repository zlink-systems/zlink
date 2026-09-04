# Systems.Zlink

Systems.Zlink is the .NET binding package for zlink.

The package exposes the public API under the `Systems.Zlink` namespace and
ships the native zlink runtime files needed by supported platforms.

Routed receive results expose `Received.Send(...)` for sending a normal routed
message back over the original receive context. Request messages also keep
`Received.Reply(...)`; `Send` does not use request-reply state.

Create caller-owned receive buffers with `Received.Create()` and reuse the
same instance across `Recv(...)` calls when draining hot paths.

## Send back-pressure and completion

A normal SEND admission returns completion ID 0 and produces no completion.
When a DONTWAIT SEND is back-pressured, Core reports `EAGAIN` and retains the
nonzero wait token, user context, and target routing id, but not the payload.
The awaitable send helper keeps the exact packet, waits for `POLLOUT`, drains
the completion queue, and retries that packet only after the matching
`WRITABLE` token, context, and routing id arrive. `POLLCOMPLETION` remains the
request-completion signal; it is not a successful-SEND notification.

Use `Send().Message(...).TrySubmit()` for a synchronous DONTWAIT attempt. It
returns `false` on `BACKPRESSURED`/`EAGAIN` and leaves every message with the
caller; other failures throw `ZlinkSubmitException`.

An external poll loop driving awaitable SEND retries registers both `POLLOUT`
and `POLLCOMPLETION`: the former reports credit and the latter transfers sole
completion-queue drain ownership to that poller. The binding filters a
WRITABLE-only wake from the caller-visible `POLLCOMPLETION` result.

## Native library loading

The binding first honors `ZLINK_LIBRARY_PATH`, then searches packaged native
runtime locations, and finally falls back to well-known library names for the
current platform. `ZLINK_LIBRARY_PATH` is intended for development, diagnostics,
and trusted deployments where the process environment is controlled by the
operator.

Do not allow untrusted users to set `ZLINK_LIBRARY_PATH` for a privileged
service process. Services with a strict loading policy should deploy the native
library in a fixed application directory and control that directory's ownership
and write permissions.

## CancellationToken policy

`CancellationToken` is part of an API only when the operation can actually
wait and the binding can observe cancellation while it waits. Awaitable send
and request operations keep optional tokens because they can wait for writable
retry notifications, request completions, replies, timeouts, or queue progress.

Synchronous builders, immediate configuration calls, and callback-style submit
methods do not take a token. A token must not be added only to call
`ThrowIfCancellationRequested()` before a non-cancelable operation.

Project repository: https://github.com/zlink-systems/zlink
