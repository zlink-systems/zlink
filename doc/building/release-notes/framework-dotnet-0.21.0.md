[English](./framework-dotnet-0.21.0.md) | [한국어](./framework-dotnet-0.21.0.ko.md)

# ZLink .NET Framework 0.21.0 Release Notes

Framework 0.21.0 uses binding 1.2.2 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

- None. The public contract is the same as 0.20.0.

## Common Changes

- The tutorial server reports fanout subscriber readiness at `GET /fanout/broadcast/ready` (200/503) and the tutorial CI waits for it before publishing the broadcast; fanout keeps no pre-connection events, so a publish must follow subscriber readiness. (#839)
- ZoneWorld `run_sample.sh` includes child runners and the browser process in its single EXIT cleanup, so a failed run leaves no Redis container or process behind. (#823)
- The runtime and the public contract are unchanged from 0.20.0. This release cleans up the tutorial, samples, quickstart and the repository tooling.
- Quickstart, tutorial and samples are obtained from the per-language examples repository (`zlink-<lang>-examples`); the mirror workflow preserves executable bits (#831) and the README starts with an English | 한국어 switch.
- The tutorial CI C++ job keeps the vcpkg binary cache in the Actions cache. (#852)
- google-java-format 1.27.0 runs the Java and Kotlin format check under JDK 25. (#798)
- The unused v11 public-contract trace generator and inventories are removed. (#747)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.21.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.21.0" />
```

The release tag is [`framework-dotnet/v0.21.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.21.0).
