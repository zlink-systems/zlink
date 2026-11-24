[English](./framework-dotnet-0.34.1.md) | [한국어](./framework-dotnet-0.34.1.ko.md)

# ZLink .NET Framework 0.34.1 Release Notes

Framework 0.34.1 is a hotfix for one defect in 0.34.0. It keeps .NET binding 1.26.0 and Core 1.26.0.

## Bug fixes

- When an Actor and its bound session were on different nodes, pushes the Actor sent in order to that session could reach the client in reverse order. This was a regression in 0.34.0. A relayed push now enters the session's send order in the order it was received. (#5459, #5469)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.34.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.34.1" />
```

The release tag is [`framework-dotnet/v0.34.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.34.1).
