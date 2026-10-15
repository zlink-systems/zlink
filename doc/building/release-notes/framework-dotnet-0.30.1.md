[English](./framework-dotnet-0.30.1.md) | [한국어](./framework-dotnet-0.30.1.ko.md)

# ZLink .NET Framework 0.30.1 Release Notes

Framework 0.30.1 uses .NET binding 1.19.2 and Core 1.19.0.

## Changes

- The server Framework now uses zlink License 1.0; versions before 1.0.0 remain free on any number of machines without a key or paid license. Client connectors, shared client code, samples, tutorials, quickstarts, and the HTTP client use Apache-2.0; licenses for releases through 0.30.0 are unchanged. (#1681)
- Public API addition: request results expose an `Admitted` stage through `ZLinkRequest<TReply>`. Callers can wait for source-local acceptance separately from the reply; failures before admission fail both stages with the same cause. (#1682)
- Reduced intermediate payload copies, repeated topology reads, and Protobuf writer and request-observation allocations; binding admission is awaited only after classifying the result as backpressured. A blocked caller continuation no longer delays other request completions. (#1689, #1696, #1747)
- Runtime code is now optimized in Debug builds as well, avoiding unnecessary state-machine allocations for synchronously completed async calls. (#1706)

## Defect Fixes

- Fixed inconsistent handling of requests with malformed envelopes across languages. An error reply is sent when the header is a syntactically valid JSON object with a valid correlation ID and a reply token; otherwise the failure is recorded without a reply. (#1671)
- Aligned error classification across languages so invalid Spot Configure handler and subscription registrations are rejected as `InvalidOperation` before the activation becomes Ready. (#1715)
- Relocation now uses the new owner generation returned by the Store CAS and carries sealed Session bindings. This fixes paths that used inconsistent generations or omitted binding state when finalizing Session routes after relocation. (#1807)
- Fixed teardown exceptions when Actor Destroy, ownership loss, and Join cleanup overlapped, and Message Follow routes being exposed before they were usable. Teardown for an incarnation and cutover route availability are each committed at one boundary. (#1666, #1669)
- Fixed AutoConnect startup and shutdown races causing `NullReferenceException`, or peer cleanup failing with `ObjectDisposedException` after transport disposal. Worker tokens are captured at startup, and AutoConnect is stopped before transport cleanup. (#1668, #1711)
- Fixed overlapping Spot Close, successor publication, cold activation, and shutdown exposing an unready Spot to retained work or hanging teardown. Close acceptance and joining are serialized, shutdown waits for accepted activations and published relocation attempts to finish cleanup, and cancelled initial Location claims release confirmed leases. (#1670, #1677, #1773, #1794)
- Fixed RouteMesh sends backing up until the next poll after permit grants, and duplicate multicast child-permit acquisition. Grants wake the receive owner, one source acquires child permits, unspecified HWM settings use Core defaults, and request cancellation removes queue waiters before completing the caller. (#1684, #1794)
- Fixed the Stream Connector's default JSON codec using a different format from server typed JSON. Both now use `framework-json-v1`, including 64-bit integer strings, enum names, and required, null, and duplicate-field validation. (#1703)
- Fixed cleanup of a physically terminated Session sending Actor unbound again and causing shutdown to end in `ForceStopped/TeardownFailed`. Unbound is no longer submitted during transport termination. (#1708)
- Fixed an accepted handler's `BoundSession.Send` being rejected again after a relocation or shutdown seal. Local Actor sends now also pass through Actor mailbox acceptance and FIFO ordering, preventing premature success or overtaking earlier messages. (#1709, #1744)
- Fixed node startup failing when an assembly contained an open generic handler definition even if only closed types were registered. Non-instantiable scan candidates are skipped; explicitly registering an open generic remains a configuration error. (#1736)
- Fixed deferred Actor Join recovery rejecting empty replies because metadata was absent. Empty replies are now recovered without metadata. (#1746)
- Fixed duplicate Renew calls for the same descriptor revision overwriting a successful publication with a stale result and failing host startup. Descriptor publication is now finalized through one path. (#1807)
- Fixed blocking polls from multiple ClientServer hosts exhausting the ThreadPool and socket close failing with `EBUSY` during receive by letting dedicated receive threads close their sockets after the loops end. Accepted replies are sent until the remaining drain deadline; admission recording order, control reception before admission, and first owner-lease heartbeat timing are also corrected. (#1812, #1822)
- Fixed durable requests and relocation batches or cutover stalling during route replacement by resubmitting them on a different observed route once ready. Ordinary records received before admission are discarded without reply, crossed HELLOs receive ADMIT using the original snapshot, and intervening descriptor changes are published through UPDATE after ADMIT. (#1825, #1870)
- Aligned a relocation authority read as `Missing` after target shutdown with the expired-owner terminal instead of `DataLost`. Removed authority-generation rechecks outside the Session owner's responsibility during Session route finalization. (#1847)
- Fixed cancellation callbacks during connection-intent removal waiting for their own state lane and hanging Dispose, or ending durable requests as `NotFound`. Retirement is committed in the intent-removal turn, external cleanup runs outside it, and lifecycle termination consistently reports `Unavailable`. (#1849, #1861)
- Uses .NET binding 1.19.2, fixing memory corruption when compacting GC invalidated native addresses of small payloads, and regressions that accessed released storage after repeated Dispose or Message reuse. (#1841, #1858)
- Removed startup checks that called `Configure()` on a temporary Spot without an identity. Registration is checked during actual activation, so `Configure()` can use the actual Spot identity. (#1715)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.30.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.30.1" />
```

The release tag is [`framework-dotnet/v0.30.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.30.1).
[English](./framework-dotnet-0.30.1.md) | [한국어](./framework-dotnet-0.30.1.ko.md)
