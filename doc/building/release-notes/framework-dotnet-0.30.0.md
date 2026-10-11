[English](./framework-dotnet-0.30.0.md) | [한국어](./framework-dotnet-0.30.0.ko.md)

# ZLink .NET Framework 0.30.0 Release Notes

Framework 0.30.0 uses .NET binding 1.19.0 and Core 1.19.0.

## Changes

- Classic fanout publisher socket configuration now comes only from the channel's fanout builder, before startup. Each channel can choose its own send timeout and NoDrop setting. Every publisher starts with linger 0 and keeps it on shutdown. The Framework no longer sets send HWM and uses the Core value. (#1586)
- When same-host Spot and Actor calls or local publishes wait beyond the application job queue's effective maximum, the runtime logs one `zlink.runtime.host.local_job_backlog_exceeded` Warning per waiting episode. Remote ingress waiters are not counted, and admission and execution behavior are unchanged. (#1466)
- Removed public APIs: `IZLinkFrameworkOptions.DefaultSocketSendTimeout`, `IZLinkMeshNodeBuilder.ConfigureSpotPublisher()`, `IZLinkSpotPublisherConfig`, `IZLinkSpotSubscriberConfig`. Set the publisher send timeout with `SetSendTimeout` on the channel's fanout builder. (#1586)

## Defect Fixes

- Fixed re-creating a User or Instance Spot failing with `location owner lease is unavailable` after its owner node was killed. When the relocation policy registered on the reserving node is Disabled, the runtime releases the Ready record whose owner lease has ended and creates a new generation. A User Spot is released only when it has no Store membership. Ordinary messages and Find never reclaim a record. (#1575)
- A tokenless request refused because no completion slot is available now reports `Unavailable`. A real request timeout still reports `DeadlineExceeded`. (#1602)
- RouteMesh and ClientServer now refresh the peer liveness deadline (15 seconds) with every record received on the current connection. (#1466)
- Host receive-flow state (PAUSED) is no longer applied to ClientServer Client DEALER sockets. It applies only to sockets that receive requests for this host (RouteMesh ROUTER, ClientServer Server ROUTER). This fixes servers being unable to send replies to a busy client host. (#1466)
- Fixed liveness expiry (no receipt for 15 seconds) ending admission without closing the physical connection. The expired target is now removed, so sends waiting on that connection terminate. (#1466)
- Fixed a request to an already Ready Instance Spot failing with `Stored Instance activation is missing` (DataLost) when two requests for the same Spot overlapped. A local Spot with the current owner generation is now admitted before the recovery record is read. (#1575)
- Fixed Actor request completion-slot exhaustion being reported as "target route is not connected". The submission failure cause is preserved to the shared mapper, which projects it once, and durable requests no longer replay a capacity refusal that has no wait token. (#1466)
- Fixed the RouteMesh receive loop reserving receive permits when only completions arrived, and keeping PollIn registered while at the permit limit. (#1466)
- Uses Core 1.19.0. Core 1.18.0 wrote receive-flow controls to a pipe outside the socket turn, so a concurrent send on another thread could end the process with a `pipe.cpp` assertion or SIGSEGV. The Framework calls this transition under host pressure, so loaded servers could hit it. (#1622)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.30.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.30.0" />
```

The release tag is [`framework-dotnet/v0.30.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.30.0).
[English](./framework-dotnet-0.30.0.md) | [한국어](./framework-dotnet-0.30.0.ko.md)
