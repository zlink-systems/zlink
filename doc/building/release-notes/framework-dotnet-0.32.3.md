[English](./framework-dotnet-0.32.3.md) | [한국어](./framework-dotnet-0.32.3.ko.md)

# ZLink .NET Framework 0.32.3 Release Notes

Framework 0.32.3 is a hotfix for stalled application progress during STREAM control processing. It also includes the ClientServer liveness ACK fix from 0.32.2. It keeps .NET binding 1.23.0 and Core 1.23.0.

## Bug fixes

- STREAM controls could wait for an application turn or connection readiness while retaining ingress permits, stalling Session and Actor requests. Controls run on the existing session state lane and are processed when the session is absent, preparing its connection, or closing. Transport termination after a protocol error also proceeds without waiting for an application turn. (#4345, #4552)
- A failed ClientServer client liveness ACK submission reports an error. Transport monitoring, liveness, and Core reconnection determine connection termination and reconnection, preventing requests from permanently failing through reuse of a closed connection. This fix has been included since 0.32.2. (#4345, #4424)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.32.3" />
<PackageReference Include="Zlink.HttpClient" Version="0.32.3" />
```

The planned release tag is `framework-dotnet/v0.32.3`. Tagging and package publication follow the merge of #4552.
