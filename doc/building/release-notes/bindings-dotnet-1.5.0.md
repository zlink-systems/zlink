[English](./bindings-dotnet-1.5.0.md) | [한국어](./bindings-dotnet-1.5.0.ko.md)

# ZLink .NET binding 1.5.0 release notes

This release aligns the .NET binding package with Core 1.5.0.

## Changes

- The NuGet package version is 1.5.0 for the Core 1.5.0 release.
- Core 1.5.0 fixes loading `libzlink` with late `dlopen()` in hosts with little static TLS surplus. The Beast WebSocket secure PRNG now stores its per-thread state in a heap object behind an 8-byte owning pointer instead of a 112-byte `thread_local` object, reducing PT_TLS usage from 312 to 200 bytes. The public C API and ABI are unchanged.
- This release changes no .NET binding behavior; it aligns the binding version with Core 1.5.0.

## Verification

- The release preflight verifies the .NET binding version, release notes, and NuGet package metadata.

The release tag is `dotnet/v1.5.0`.

