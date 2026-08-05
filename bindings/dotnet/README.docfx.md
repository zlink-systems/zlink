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
