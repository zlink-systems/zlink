# zlink .NET Binding API Reference

This reference is generated from the C# source in `bindings/dotnet/src/Zlink/`.

## Prerequisites

```bash
dotnet tool install -g docfx
```

## Generate

```bash
cd bindings/dotnet
docfx docfx.json
```

Generated HTML entrypoint:

```text
bindings/dotnet/_site/index.html
```

## Scope

- Public types in the `Systems.Zlink` namespace
- Socket types, message types, domain objects
- Service wrappers in `Systems.Zlink.Service`
- Internal/native types (`Systems.Zlink.Native`) are excluded

## Contract Notes

- Public resource-owning types support both `Dispose()` and `DisposeAsync()`.
- `SocketBase.MonitorOpen()` and `Discovery.MonitorOpen()` default to `All`
  when the event mask is omitted.
- A successful SEND returns completion ID 0 and produces no completion.
- Core does not retain the payload of a back-pressured DONTWAIT SEND. The
  awaitable send helper keeps the exact packet, waits for `POLLOUT`, drains the
  completion queue, and retries only after the matching `WRITABLE` token,
  context, and routing id arrive.
- `Send().Message(...).TrySubmit()` returns `false` for
  `BACKPRESSURED`/`EAGAIN` and leaves its messages with the caller.
- External loops register `POLLOUT | POLLCOMPLETION`; the latter transfers sole
  queue-drain ownership, while WRITABLE-only wakes remain caller-visible as
  `POLLOUT` rather than `POLLCOMPLETION`.
- `POLLCOMPLETION` remains the request-completion signal; it is not a
  successful-SEND notification.
