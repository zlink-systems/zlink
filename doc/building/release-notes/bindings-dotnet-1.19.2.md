[English](./bindings-dotnet-1.19.2.md) | [한국어](./bindings-dotnet-1.19.2.ko.md)

# ZLink .NET binding 1.19.2 release notes

Uses Core 1.19.0.

## Changes

- In 1.19.1, `Message.Allocate` borrowed user-owned `Message` instances from the receive wrapper pool. Disposing a returned wrapper again freed its native storage, causing later `Message` operations to throw `ObjectDisposedException`. `Allocate` no longer uses the pool, and disposing the same `Message` multiple times is safe. There is no public API change. Per-call allocations in `Allocate` increased by 32 B managed and 64 B native (#1858).
