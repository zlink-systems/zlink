[English](./bindings-dotnet-1.7.0.md) | [한국어](./bindings-dotnet-1.7.0.ko.md)

# ZLink .NET binding 1.7.0 release notes

This release aligns the .NET binding package with Core 1.7.0.

## Changes

- The NuGet package version is 1.7.0 and uses Core 1.7.0.
- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- This release changes no .NET binding behavior.

## Verification

- The release preflight verifies the .NET binding version, release notes, and NuGet package metadata.

The release tag is `dotnet/v1.7.0`.
