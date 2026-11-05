[English](./framework-dotnet-0.32.0.md) | [한국어](./framework-dotnet-0.32.0.ko.md)

# ZLink .NET Framework 0.32.0 Release Notes

Framework 0.32.0 uses the .NET binding 1.23.0 and Core 1.23.0. (#3843)

## Behavior Changes

- Unknown control packets and heartbeat payload versions no longer close the connection. (#3773, #3790)
- The Actor owner rejects duplicate or out-of-order relayed session sequences. Stale-relay records use the session owner node as `source_rid`. (#1721, #3763, #3799)
- `MessageContext.receivedAt` is the UTC time when Framework accepts a message. DTO properties without defaults are required in framework-json-v1. Duplicate envelope header fields are rejected. (#2029, #3602, #1705, #3592, #3376, #3396)
- Duplicate-bind decisions go through the owner Actor's `onDuplicateBind` callback. (#2205, #3640)

## Defect Fixes

- Late physical results invalidate a route cache entry only when its fence still matches; cache invalidation follows the trigger table in 08-routing §2. Forced-stop Store cleanup uses one deadline and sweeps by authority; cleanup releases reservations when target lifecycles end. (#3571, #3771, #2989, #3409, #2646, #3462, #3583, #3697)
- Relocation terminal results are recorded in message flow, with cutover flow and Bingo join/observer policy preserved. (#2465, #3621)
- Callback failures preserve the original exception. A malformed request with readable correlation receives a terminal protocol error. (#2098, #3429, #2512, #3418)
- Instance-intent requests reject stale routes before admission and follow current authority within the request deadline. Liveness cleanup completes peer cleanup and notification even when disconnect fails. (#3353, #3442, #2370, #3434)
- Auto-connect excludes only descriptors for the same endpoint and waits for publication before peers become visible. It does not list peers when publishing its own descriptor fails. (#2155, #3433, #3569, #3664, #3723, #3736)
- Late terminal replies release the Actor slot and use a Session reply after binding completes. An accepted Instance intent remains visible during Close. (#2757, #3419, #3606, #3628)
- Location, publication, and activation deadlines use the injected `TimeProvider`. (#3767, #3782)
- Request completion continuations run asynchronously, and receive progress is preserved after preemption. (#3610, #1674, #3413)
- A receive-claim authority-check failure is settled for its record so the remaining batch records can proceed. (#3352, #3362)

## Performance Improvements

- Request-response conversion is composed into the final receiver, removing a forwarding async layer. (#1674, #3481)

## Tests and CI

- The relocation-preparation test synchronizes worker-pool and trace-export observations. (#3761, #3766)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.32.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.32.0" />
```

The release tag is [`framework-dotnet/v0.32.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.32.0).
