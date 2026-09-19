[English](./bindings-dotnet-1.2.1.md) | [한국어](./bindings-dotnet-1.2.1.ko.md)

# ZLink .NET binding 1.2.1 release notes

This release runs on Core 1.2.0. Core is unchanged; only the packaging is fixed.

## Changes

- The NuGet package now ships `runtimes/win-x64/native/zlink.dll`. Version 1.2.0 carried only the Linux x64 runtime, causing `DllNotFoundException` on Windows. Windows x64 now works without a separate Core installation. (#702)

## Verification

- The `Zlink.1.2.1.nupkg` file listing contains `runtimes/win-x64/native/zlink.dll` and the three Linux x64 runtime files.

The release tag is `dotnet/v1.2.1`.
