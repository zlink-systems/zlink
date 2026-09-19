[English](./bindings-dotnet-1.2.2.md) | [한국어](./bindings-dotnet-1.2.2.ko.md)

# ZLink .NET binding 1.2.2 release notes

This release runs on Core 1.2.0. Core is unchanged; only the packaging is fixed.

## Changes

- `runtimes/win-x64/native/` in the NuGet package now carries the OpenSSL (`libcrypto-3-x64.dll`, `libssl-3-x64.dll`) and MSVC runtime DLLs that `zlink.dll` depends on. 1.2.1 shipped `zlink.dll` alone, so `LoadLibrary` could not resolve its dependencies and Windows still threw `DllNotFoundException`. The package carries every `bin/*.dll` of the Core Windows archive, the same set as the Node prebuild (`prebuilds/win32-x64`). (#702)

## Verification

- The `Zlink.1.2.2.nupkg` file listing contains `runtimes/win-x64/native/zlink.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`, the MSVC runtime DLLs, and the three Linux x64 runtime files.
- Whether the distributed .NET tutorial zip starts on a Windows runner is checked by the next framework release's `standalone-zips` verification.

The release tag is `dotnet/v1.2.2`.
