[English](./framework-cpp-0.32.0.md) | [한국어](./framework-cpp-0.32.0.ko.md)

# ZLink C++ Framework 0.32.0 Release Notes

Framework 0.32.0 uses C++ binding 1.23.0 and Core 1.23.0. (#3843)

## Changes

- The C++ server and Stream Connector follow the Stream Connector control-frame rules. Unknown control packets and heartbeat payload versions no longer close the connection. (#3773, #3820)
- C++ rejects duplicate or lower session sequences on relayed Actor messages. (#1721, #3783)
- `MessageContext.receivedAt` is the UTC time when Framework accepts a message. DTO properties without defaults are required in framework-json-v1. (#2029, #3602, #1705, #3592)
- Duplicate envelope header fields are rejected. Duplicate-bind decisions go through the owner Actor's `onDuplicateBind` callback. (#3376, #3396, #2205, #3640)

## Defect Fixes

- Spot creation, Store scans, mesh and fanout publication, and Actor authority updates return lane tasks instead of blocking infrastructure workers. Auto-connect updates also progress without waiting on worker completion. (#2960, #3748, #3743, #3663, #3758, #3770, #3841, #3861)
- Fanout descriptor cancellation uses the domain stop source. Host shutdown closes Entry Spots through the Close gate before owner and transport teardown. (#3850, #3856, #3776, #3786)
- A MeshNode with role None runs without an in-process Location Store. (#3721, #3752)
- Coroutines no longer run inside capturing lambdas; their inputs and request lifecycle state remain in the coroutine frame. (#3691, #3775, #3594, #3696)
- Legacy Spot and node requests retain one deadline, and resolver waits end on deadline or shutdown. (#3580, #3660)
- Stream Connector connect and cancel socket operations run on the same strand. (#3534, #3545)
- Late physical results invalidate a route cache entry only when its fence still matches; cache invalidation follows the trigger table in 08-routing §2. Forced-stop Store cleanup uses one deadline and sweeps by authority; cleanup releases reservations when target lifecycles end. (#3571, #3771, #2989, #3409, #2646, #3462, #3583, #3697)
- Relocation terminal results are recorded in message flow, with cutover flow and Bingo join/observer policy preserved. (#2465, #3621)
- Callback failures preserve the original exception. A malformed request with readable correlation receives a terminal protocol error. (#2098, #3429, #2512, #3418)
- Request results keep one owner from native completion through typed reply; decode failures preserve the original exception and authority lookup failures preserve their classification. A failed native Spot target is settled only for its claim record. (#3600, #3472, #3507, #3358, #3384, #3394, #3403)
- Actor gateway storage breaks a shared_ptr ownership cycle. (#3506, #3520)
- Instance-intent requests reject stale routes before admission and follow current authority within the request deadline. Liveness cleanup completes peer cleanup and notification even when disconnect fails. (#3353, #3442, #2370, #3434)
- Auto-connect excludes only descriptors for the same endpoint and waits for publication before peers become visible. Late terminal replies release the Actor slot and use a Session reply after binding completes. (#2155, #3433, #3569, #3664, #2757, #3419)

## Performance Improvements

- Receive and Spot reply paths no longer copy payloads. Ready send results stay inline, and canonical envelope headers use the fast path. (#3693, #3768, #1672, #3518, #3375)
- Direct Node and Channel records skip the Location owner-lease query. Trace-disabled message-flow sites skip trace work. (#3607, #3568, #3623)
- The worker-pool size follows the number of CPUs available to the process. (#3555, #3557)

## Tests and CI

- Regex source scans moved out of target_contract, and the capturing-coroutine scanner runs as an optimized gate. (#3826, #3831, #3838)

## Install

Select a platform Framework archive from the [GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.32.0). A server Framework requires C++ binding 1.23.0 and Core 1.23.0. A client that uses only the Stream Connector does not require Core or the binding.

The release tag is [`framework-cpp/v0.32.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.32.0).
