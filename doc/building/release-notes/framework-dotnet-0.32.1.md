[English](./framework-dotnet-0.32.1.md) | [한국어](./framework-dotnet-0.32.1.ko.md)

# ZLink .NET Framework 0.32.1 Release Notes

Framework 0.32.1 is a hotfix for one defect in 0.32.0. It keeps .NET binding 1.23.0 and Core 1.23.0.

## Bug fixes

- On a ClientServer Server channel, when expiry cleanup of a disconnected client got `NOT_FOUND` from the transport, the whole channel receive loop ended and requests from other healthy connections on the same channel got no reply. This was a regression in 0.31.0. `NOT_FOUND` now counts as an already closed peer, and any other disconnect error is reported as a channel error while the other peers keep being processed. (#4301, #4304)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.32.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.32.1" />
```

The release tag is [`framework-dotnet/v0.32.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.32.1).
