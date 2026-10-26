[English](./framework-dotnet-0.30.3.md) | [한국어](./framework-dotnet-0.30.3.ko.md)

# ZLink .NET Framework 0.30.3 Release Notes

Framework 0.30.3 uses .NET binding 1.20.0 and Core 1.20.0.

## Defect Fixes

- Fixed `GetOrCreate` failing to Join a User Spot while that Spot was closing. (#2672)
- Fixed an exception when calling `Close` with a stale `SpotRef` after the Spot had closed. A second `Close` call joins an in-progress `Close` and receives the same result. (#2675)
- Fixed `ShutdownAsync` remaining in `Draining` after a cross-node Actor Join was rejected. (#2682)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.30.3" />
<PackageReference Include="Zlink.HttpClient" Version="0.30.3" />
```

The release tag is [`framework-dotnet/v0.30.3`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.30.3).
