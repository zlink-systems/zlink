[한국어](./framework-dotnet-0.28.0.ko.md) | [English](./framework-dotnet-0.28.0.md)

# ZLink .NET Framework 0.28.0 Release Notes

Framework 0.28.0 uses .NET binding 1.15.0 and Core 1.15.0. (#1460)

## Contract Changes

- **A one-way send has no time limit and no caller cancellation.** This covers RouteMesh node/Channel, Spot, Actor, ClientServer, bound session and session Actor relay sends, per-target submission of a committed Logical Multicast, and STREAM send/reply. The wait ends only with admission after capacity recovers (normal completion), route removal while waiting (`Unavailable`), or socket close/runtime shutdown (`ShuttingDown`). Core 1.15.0 wait tokens have no deadline and binding cancellation ends only the caller's wait, so ending a send by time or cancellation could let the message go out after a "not sent" result. Use a request when you need the delivery result. (#1461)
- **The request timeout bounds outbound admission and the reply wait together.** When a timeout or cancellation ends the caller's wait, the request can still be sent later by a binding resubmission that already started; its reply is discarded. (#1461)
- **Only the Classic fanout publisher uses a send timeout.** Its value rules and 1-second default are unchanged. (#1461)
- The activation deadline of a one-way cold activation of a Missing Instance Spot is the send submission start time plus the source MeshNode's default request timeout. It applies only to the target's activation work and doesn't end the caller's send wait. (#1461)
- Actor and User Spot creation transitions (Reserve, Commit, Abort) verify both the target owner lease and the StoreVersion of the target MeshNode descriptor as first read. Completion of an already accepted creation proceeds even if the target is no longer Serving. (#1432)

## Compatibility-Affecting Changes

- Removed the token argument from send/reply/relay `Async(CancellationToken)` and `RelayAsync(..., CancellationToken)` (`IZLinkSendCall`, `IZLinkSpotSendCall`, `IZLinkActorSendCall`, `IZLinkBoundSessionSendCall`, `IZLinkSessionSendCall`, `IZLinkSessionReplyCall`, `IZLinkSessionActor.RelayAsync`). Removed `IZLinkSessionSendCall.Timeout(...)` and `SendTimeout` from `IZLinkSocketConfig`, `IZLinkStreamSocketConfig`, and `IZLinkMeshNodeSocketConfig`. `DefaultSocketSendTimeout` applies only to the Classic fanout publisher. (#1461)

## Defect Fixes

- Examples mirror: the Java/Kotlin ZoneWorld Windows runner now finds the shared process script in the mirror. (#1035)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.28.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.28.0" />
```

The release tag is [`framework-dotnet/v0.28.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.28.0).
