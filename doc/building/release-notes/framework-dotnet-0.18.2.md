[English](./framework-dotnet-0.18.2.md) | [한국어](./framework-dotnet-0.18.2.ko.md)

# ZLink .NET Framework 0.18.2 Release Notes

Framework 0.18.2 uses binding 1.2.2 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The public API does not change. The only difference from 0.18.1 is the pinned binding version.

## Fixes

- Pins binding 1.2.2. 1.2.1 shipped only `zlink.dll` in the NuGet package, so `LoadLibrary` could not resolve its dependencies (OpenSSL `libcrypto-3-x64.dll`/`libssl-3-x64.dll` and the MSVC runtime) and the published packages alone still threw `DllNotFoundException` on Windows. 1.2.2 carries every `bin/*.dll` of the Core Windows archive under `runtimes/win-x64/native/`. For the rest of the 0.18.1 changes see the [0.18.1 release notes](./framework-dotnet-0.18.1.md). (#702)

## Installation

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.18.2
```

The release tag is [`framework-dotnet/v0.18.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.18.2).
