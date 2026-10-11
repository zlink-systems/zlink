[English](./framework-java-0.30.0.md) | [한국어](./framework-java-0.30.0.ko.md)

# ZLink Java·Kotlin Framework 0.30.0 Release Notes

Framework 0.30.0 uses Java·Kotlin binding 1.19.0 and Core 1.19.0.

## Changes

- Classic fanout publisher socket configuration now comes only from the channel's fanout builder, before startup. Each channel can choose its own send timeout and NoDrop setting. Every publisher starts with linger 0 and keeps it on shutdown. The Framework no longer sets send HWM and uses the Core value. (#1586)
- When same-host Spot and Actor calls or local publishes wait beyond the application job queue's effective maximum, the runtime logs one `zlink.runtime.host.local_job_backlog_exceeded` Warning per waiting episode. Remote ingress waiters are not counted, and admission and execution behavior are unchanged. (#1466)
- Removed public APIs: `ZLinkMeshNodeBuilder.configureSpotPublisher()`, `ZLinkSpotPublisherConfig`. Set the publisher send timeout on the channel's fanout builder. (#1586)

## Defect Fixes

- Fixed re-creating a User or Instance Spot failing with `location owner lease is unavailable` after its owner node was killed. When the relocation policy registered on the reserving node is Disabled, the runtime releases the Ready record whose owner lease has ended and creates a new generation. A User Spot is released only when it has no Store membership. Ordinary messages and Find never reclaim a record. (#1575)
- A tokenless request refused because no completion slot is available now reports `Unavailable`. A real request timeout still reports `DeadlineExceeded`. (#1602)
- RouteMesh and ClientServer now refresh the peer liveness deadline (15 seconds) with every record received on the current connection. (#1466)
- Fixed Fanout and ClientServer `listenerStatus` publishing a wildcard address. The endpoint the binding actually bound is published. (#1466)
- Fixed receive rounds blocking infrastructure progress on the same router while waiting for an ordinary permit or control send admission. (#1466)
- Fixed a race where native submission could start while a raw service port was closing, making close fail with `EBUSY`. (#1466)
- Host receive-flow state (PAUSED) is no longer applied to ClientServer Client DEALER sockets. It applies only to sockets that receive requests for this host (RouteMesh ROUTER, ClientServer Server ROUTER). This fixes servers being unable to send replies to a busy client host. (#1466)
- Fixed liveness expiry (no receipt for 15 seconds) ending admission without closing the physical connection. The expired target is now removed, so sends waiting on that connection terminate. (#1466)
- Fixed a request to an already Ready Instance Spot failing with `Stored Instance activation is missing` (DataLost) when two requests for the same Spot overlapped. A local Spot with the current owner generation is now admitted before the recovery record is read. (#1575)
- Uses Core 1.19.0. Core 1.18.0 wrote receive-flow controls to a pipe outside the socket turn, so a concurrent send on another thread could end the process with a `pipe.cpp` assertion or SIGSEGV. The Framework calls this transition under host pressure, so loaded servers could hit it. (#1622)
- Fixed host shutdown not waiting for the Store cleanup of accepted Instance activations (the first message's terminal record and recovery record deletion). The Spot close stage now waits for it within the drain deadline. (#1575)
- Fixed Instance Spot requests that ended with a moved owner's refusal (SpotMoving) or a framework Unavailable leaving the Ready route cache in place, so later requests kept reaching the previous owner. Such a terminal now invalidates the cache and the next independent request resolves the current owner. The ended request is not resubmitted. (#1575)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.30.0")
    implementation("systems.zlink:zlink-http-client:0.30.0")
}
```

The release tag is [`framework-java/v0.30.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.30.0).
[English](./framework-java-0.30.0.md) | [한국어](./framework-java-0.30.0.ko.md)
