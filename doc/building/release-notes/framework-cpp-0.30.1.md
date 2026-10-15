[English](./framework-cpp-0.30.1.md) | [한국어](./framework-cpp-0.30.1.ko.md)

# ZLink C++ Framework 0.30.1 Release Notes

Framework 0.30.1 uses C++ binding 1.19.0 and Core 1.19.0.

## Changes

- The server Framework now uses zlink License 1.0; versions before 1.0.0 remain free on any number of machines without a key or paid license. Client connectors, shared client code, samples, tutorials, quickstarts, and the HTTP client use Apache-2.0; licenses for releases through 0.30.0 are unchanged. (#1681)
- Public API addition: request results expose an `admitted()` stage through `request_task_t<TReply>`. Callers can wait for source-local acceptance without waiting for the reply; failures before admission fail both stages with the same cause. (#1682)
- Public API addition: `framework_runtime_status_t.safe_to_shutdown` exposes whether a relocation source can shut down. Status observations are also published when source cleanup completes. (#1853)
- Reduced per-message waits for topology and owner-lease lane reads and duplicate completion scheduling. Received headers are decoded once at ingress, and payloads are transferred without intermediate copies. (#1650, #1662, #1665)

## Defect Fixes

- Fixed inconsistent handling of requests with malformed envelopes across languages. An error reply is sent when the header is a syntactically valid JSON object with a valid correlation ID and a reply token; otherwise the failure is recorded without a reply. (#1671)
- Aligned error classification across languages so invalid Spot Configure handler and subscription registrations are rejected as `InvalidOperation` before the activation becomes Ready. (#1715)
- Relocation now uses the new owner generation returned by the Store CAS and carries sealed Session bindings. This fixes paths that used inconsistent generations or omitted binding state when finalizing Session routes after relocation. (#1807)
- Fixed requests joining the same Instance Spot cold activation failing with `DataLost` after the first request deleted its recovery record. Joiners use the accepted activation input while retaining metadata and payload consistency checks. (#1653)
- Fixed macOS arm64 prebuilt builds failing because Apple libc++ did not support the atomic shared-pointer form used by the runtime. (#1667)
- Fixed deferred Actor Join recovery rejecting an empty reply because reply metadata was absent. Empty replies are now preserved and recovered without metadata. (#1746)
- Fixed `admitted()` succeeding when a ClientServer request ended before Core submission because the target was not ready or its deadline had expired. Admission and reply now fail with the same cause. (#1749)
- Fixed releasing a route client with a pending request causing its worker to wait for itself and hang builder destruction or shutdown. Execution resources are drained by runtime teardown, and workers that exit while idle are reclaimed. (#1768, #1789)
- Fixed `Yield()` releasing the execution gate before the handler actually suspended, or retaining it in facade calls. Resumption uses a new turn on the same queue, allowing repeated Yield and `Defer()` calls; waits during owner shutdown are completed by that owner. (#1789)
- Fixed cross-node bind, create, Join, and relocation stalling during route replacement by resubmitting durable requests and batches or cutover on a different observed route once ready. The same HELLO snapshot is resent until ADMIT, later descriptor changes use UPDATE, and automatic discovery avoids duplicate connections when a route or manual intent for that peer already exists. (#1814, #1851, #1871)
- Fixed local sends to a returning Actor being rejected as `NotFound` while Join completion was pending. The target is resolved by its current ActorId, and the Actor queue and execution gate determine when it runs. (#1823)
- Fixed a slow initial owner-lease claim delaying the first heartbeat. The first renewal is now scheduled from the claim operation's start time. (#1830)
- Fixed an ordinary exception in a deferred Actor Join completion callback terminating the process through `std::terminate`. The original exception is propagated, target execution failures retain their `Failed.Kind`, and normal `Rejected` results preserve optional replies for both local and remote calls. (#1844)
- Fixed source cleanup and host close continuing to wait when a relocation target shut down just after commit and its authority became `Missing`. Source work now ends with the expired-owner terminal, and Session route finalization checks only the identity and binding fields owned by the Session owner. (#1847)
- Fixed host shutdown with Actor members ending in `ForceStopped/TeardownFailed`, and failures or hangs in maintenance Entry Actor restoration and application-signaled SpotWide capture. Shutdown retires local Actors, maintenance uses the target startup Entry, and capture proceeds within its held turn. (#1853)
- Fixed User Spot creation refusals during movement being sent with a Busy terminal. `SpotMoving` now uses the Conflict terminal. (#1715)

## Install

Select a platform Framework archive from the [`framework-cpp/v0.30.1` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.1), or download it with `bootstrap.cmake`. A server Framework requires C++ binding 1.19.0 and Core 1.19.0. A client that uses only the Stream Connector does not require Core or the binding.

The release tag is [`framework-cpp/v0.30.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.1).
[English](./framework-cpp-0.30.1.md) | [한국어](./framework-cpp-0.30.1.ko.md)
