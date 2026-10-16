[English](./framework-node-0.30.1.md) | [한국어](./framework-node-0.30.1.ko.md)

# ZLink Node.js Framework 0.30.1 Release Notes

Framework 0.30.1 uses Node.js binding 1.19.0 and Core 1.19.0.

## Changes

- The server Framework now uses zlink License 1.0; versions before 1.0.0 remain free on any number of machines without a key or paid license. Client connectors, shared client code, samples, tutorials, quickstarts, and the HTTP client use Apache-2.0; licenses for releases through 0.30.0 are unchanged. (#1681)
- Public API addition: request results expose `ZLinkRequest<TReply>.admitted` to await source-local acceptance separately from the reply. Infrastructure lookups do not complete this stage, and failures before admission reach both admission and reply with the same cause. (#1682)
- Nest handler preparation and provider lookup are consolidated at registration. Normal-path exception creation, intermediate Buffers and Promises, and repeated pressure checks are reduced. Send, receive, and completion retain payload lifetime with fewer intermediate objects. (#1652, #1657, #1693)

## Defect Fixes

- Fixed inconsistent handling of requests with malformed envelopes across languages. An error reply is sent when the header is a syntactically valid JSON object with a valid correlation ID and a reply token; otherwise the failure is recorded without a reply. (#1671)
- Aligned error classification across languages so invalid Spot Configure handler and subscription registrations are rejected as `InvalidOperation` before the activation becomes Ready. (#1715)
- Relocation now uses the new owner generation returned by the Store CAS and carries sealed Session bindings. This fixes paths that used inconsistent generations or omitted binding state when finalizing Session routes after relocation. (#1807)
- Aligned the Stream Connector's default JSON codec with server `framework-json-v1` and included it in the Unity WebGL bundle. Both use shared 64-bit integer strings and typed-field validation. Also fixed the Unity package's metadata file not matching `LICENSE.md`. (#1688, #1703, #1868)
- Fixed same-node Actor Join succeeding while Store membership remained at the previous Entry, causing maintenance relocation to be rejected. Join acceptance and native membership updates now follow authority-fence validation and the Store CAS commit. (#1739)
- Fixed deferred Actor Join recovery rejecting empty replies because metadata was absent. Empty replies are preserved and recovered without metadata. (#1746)
- Fixed slow initial owner-lease claims delaying the first heartbeat, and floating-point deadline calculations preventing timely timeout completion. Renewal and remaining timeout calculations now use the same operation start time and configured budget. (#1830, #1869)
- Fixed missing relocation-source `safeToShutdown` observations and failures in host shutdown with Actor members or maintenance Entry restoration: queries and observations share one status, shutdown retires local Actors, and maintenance restores them into the target startup Entry. `findSpot` returns the current Spot generation, and Session route finalization is sent to the actual Session owner. (#1831)
- Fixed unfinished Promises accumulating on each STREAM receive-wait cycle, increasing memory use and potentially causing `Map maximum size exceeded`. One cancellable poller wait now handles both wake and shutdown. (#1839)
- Fixed cross-node Join and relocation stalling during route replacement by resubmitting durable requests and batches or cutover on a different observed route once ready. The same HELLO snapshot is reused for retransmission and replies until ADMIT, later changes use UPDATE, automatic discovery avoids duplicate connections to peers with an existing route or manual intent, and physical disconnection preserves manual intents. (#1850, #1864)
- Fixed a deferred Join whose admission could be removed by the 30-second retention timer while its restore was in progress. The target attempt that accepts PREPARE now owns the admission and prewarm and releases them only on its terminal path. (#1729)
- Fixed duplicate Spot packet and subscription registrations being accepted or ignored. Activation now rejects them as `InvalidOperation`. (#1715)

## Install

```bash
npm install @zlink-systems/framework@0.30.1 @zlink-systems/http-client@0.30.1
```

The release tag is [`framework-node/v0.30.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.30.1).
[English](./framework-node-0.30.1.md) | [한국어](./framework-node-0.30.1.ko.md)
