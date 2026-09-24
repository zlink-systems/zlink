[한국어](./framework-dotnet-0.25.0.ko.md) | [English](./framework-dotnet-0.25.0.md)

# ZLink .NET Framework 0.25.0 Release Notes

Framework 0.25.0 uses .NET binding 1.7.0 and Core 1.7.0.

## Fixes

- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The DeliveryDispatch, Bingo, and TicTacToe sample READMEs run their commands from the sample directory ([#1035](https://github.com/zlink-systems/zlink/issues/1035)).

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.25.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.25.0" />
```

The release tag is [`framework-dotnet/v0.25.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.25.0).
