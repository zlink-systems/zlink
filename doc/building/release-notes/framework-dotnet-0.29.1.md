[English](./framework-dotnet-0.29.1.md) | [한국어](./framework-dotnet-0.29.1.ko.md)

# ZLink .NET Framework 0.29.1 Release Notes

Framework 0.29.1 uses .NET binding 1.17.0 and Core 1.17.0.

## Defect Fixes

- Fixed a 0.29.0 regression where an operation that arrived while the same Instance Spot was activating failed with `Unavailable` or `stale_target`. A later operation now joins the activation in progress and is processed in arrival order after Ready. (#1571)
- Fixed operations that joined the same target activation being processed out of arrival order under load. (#1571)
- Fixed `Create` and `GetOrCreate` failing with `location owner lease is unavailable` after the owner node of an Actor was killed. For an Actor type whose factory registration disables relocation (`DisableRelocation`), the runtime releases the record whose owner lease has ended and creates a new incarnation. A type with relocation enabled still returns `Unavailable`. (#1570)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.29.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.29.1" />
```

The release tag is [`framework-dotnet/v0.29.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.29.1).
[English](./framework-dotnet-0.29.1.md) | [한국어](./framework-dotnet-0.29.1.ko.md)
