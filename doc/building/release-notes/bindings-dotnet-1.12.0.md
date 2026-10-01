[English](./bindings-dotnet-1.12.0.md) | [한국어](./bindings-dotnet-1.12.0.ko.md)

# ZLink .NET binding 1.12.0 release notes

Uses Core 1.12.0.

## Changes

- STREAM sockets do not provide connect or disconnect and provide `disconnectRid`. A RID that does not exist returns connect error NotFound (605).

## Behavior changes

- When Core returns `BUSY` during a concurrent receive, .NET reports a receive error instead of treating it as an empty receive (#1201).
- The subscription topic receive buffer grows to the length reported by Core (#1218).
- Aligns representative errnos for REQUEST results with Core: `CONFLICT` returns `EEXIST`, and `INTERNAL_ERROR` returns `EIO` (#1218, #1248).
