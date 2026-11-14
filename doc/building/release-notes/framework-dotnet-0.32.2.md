[English](./framework-dotnet-0.32.2.md) | [한국어](./framework-dotnet-0.32.2.ko.md)

# ZLink .NET Framework 0.32.2 Release Notes

Framework 0.32.2 is a hotfix for one defect in 0.32.1. It keeps using .NET binding 1.23.0 and Core 1.23.0.

## Fixes

- When a ClientServer client's liveness ACK submission failed, the client closed that connection itself, and the connection registry kept using the closed connection. On a channel that registers both Client and Server on the same node, requests could then fail with `NotConnected` permanently even though the Server was healthy. An ACK failure is now only reported as an error; closing and reconnecting stay with the transport monitor, liveness, and Core reconnect. (#4345, #4424)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.32.2" />
<PackageReference Include="Zlink.HttpClient" Version="0.32.2" />
```

The release tag is [`framework-dotnet/v0.32.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.32.2).
