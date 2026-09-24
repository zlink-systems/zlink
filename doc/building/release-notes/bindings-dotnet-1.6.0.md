[English](./bindings-dotnet-1.6.0.md) | [한국어](./bindings-dotnet-1.6.0.ko.md)

# ZLink .NET binding 1.6.0 release notes

This release aligns the .NET binding package with Core 1.6.0.

## Changes

- The NuGet package version is 1.6.0 for the Core 1.6.0 release.
- Core 1.6.0 allows ROUTER to publish a reply token after a complete REQUEST is received, even if the source pipe disconnects. The public C API and ABI are unchanged ([#1051](https://github.com/zlink-systems/zlink/issues/1051)).
- This release changes no .NET binding behavior; it aligns the binding version with Core 1.6.0.

## Verification

- The release preflight verifies the .NET binding version, release notes, and NuGet package metadata.

The release tag is `dotnet/v1.6.0`.
