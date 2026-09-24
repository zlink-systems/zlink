[한국어](./framework-dotnet-0.24.1.ko.md) | [English](./framework-dotnet-0.24.1.md)

# ZLink .NET Framework 0.24.1 Release Notes

Framework 0.24.1 uses .NET binding 1.7.0 and Core 1.7.0.

## Fixes

- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The .NET Framework contract and behavior are unchanged.

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.24.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.24.1" />
```

The release tag is [`framework-dotnet/v0.24.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.24.1).
