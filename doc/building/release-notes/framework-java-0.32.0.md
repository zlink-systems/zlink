[English](./framework-java-0.32.0.md) | [한국어](./framework-java-0.32.0.ko.md)

# ZLink Java·Kotlin Framework 0.32.0 Release Notes

Framework 0.32.0 uses the Java·Kotlin binding 1.23.0 and Core 1.23.0. (#3843)

## Behavior Changes

- Unknown control packets and heartbeat payload versions no longer close the connection. (#3773, #3790)
- Bound-session relays to an Actor are delivered in arrival order. The Actor owner rejects duplicate or out-of-order relayed session sequences, and stale-relay records use the session owner node as `source_rid`. (#3849, #3852, #1721, #3763, #3799)
- A handler executor must run or reject every task. Registration rejects an executor that silently discards a task. (#3710, #3791, #3815)
- `MessageContext.receivedAt` is the UTC time when Framework accepts a message. DTO properties without defaults are required in framework-json-v1. Duplicate envelope header fields are rejected. (#2029, #3602, #1705, #3592, #3376, #3396)
- Duplicate-bind decisions go through the owner Actor's `onDuplicateBind` callback. (#2205, #3640)

## Defect Fixes

- A late retire keeps the replacement Session binding, and a duplicate bind without a callback is accepted immediately. (#3726, #3694, #3737)
- A lane turn registers a submission to another owner before returning. Kotlin suspending Actors without a duplicate-bind callback skip the Actor gate. (#3417, #3562, #3694, #3750)
- User handler stages remain isolated from Framework cancellation. SpotNode creation uses its MeshNode instead of a lane-joined lazy lookup. (#1673, #3411, #3572)
- Late physical results invalidate a route cache entry only when its fence still matches; cache invalidation follows the trigger table in 08-routing §2. Forced-stop Store cleanup uses one deadline and sweeps by authority; cleanup releases reservations when target lifecycles end. (#3571, #3771, #2989, #3409, #2646, #3462, #3583, #3697)
- Relocation terminal results are recorded in message flow, with cutover flow and Bingo join/observer policy preserved. (#2465, #3621)
- Callback failures preserve the original exception. A malformed request with readable correlation receives a terminal protocol error. (#2098, #3429, #2512, #3418)
- Instance-intent requests reject stale routes before admission and follow current authority within the request deadline. Liveness cleanup completes peer cleanup and notification even when disconnect fails. (#3353, #3442, #2370, #3434)
- Auto-connect excludes only descriptors for the same endpoint and waits for publication before peers become visible. Late terminal replies release the Actor slot and use a Session reply after binding completes. (#2155, #3433, #3569, #3664, #2757, #3419)

## Performance Improvements

- Envelope headers are parsed once without full-header copies. Receive rotations claim work in one state-lane turn and read fixed-target routes from the published selector view. (#3613, #1673, #3402, #3416)
- Send payloads use a retained view and fewer native parts. Spring prototype handlers reuse their prepared construction path. (#1673, #3487, #3480)

## Tests and CI

- Test-injection constructors are package-private, and Java/C++ tests use scoped access seams for internals. (#2309, #3796, #3834)

## Known Limitations

- The Java `mmo-world-channels` template update E2E can intermittently leave `sessionB` running after STOPPED. (#3596)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.32.0")
    implementation("systems.zlink:zlink-http-client:0.32.0")
}
```

The release tag is [`framework-java/v0.32.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.32.0).
