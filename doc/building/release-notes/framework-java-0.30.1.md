[English](./framework-java-0.30.1.md) | [한국어](./framework-java-0.30.1.ko.md)

# ZLink Java·Kotlin Framework 0.30.1 Release Notes

Framework 0.30.1 uses Java·Kotlin binding 1.19.1 and Core 1.19.0.

## Changes

- The server Framework now uses zlink License 1.0; versions before 1.0.0 remain free on any number of machines without a key or paid license. Client connectors, shared client code, samples, tutorials, quickstarts, and the HTTP client use Apache-2.0; licenses for releases through 0.30.0 are unchanged. (#1681)
- Public API additions: Java request results expose `ZLinkRequestStage<TReply>.admitted()`, and Kotlin provides `submit()`, allowing source-local acceptance to be awaited separately from the reply. Failures before admission fail both stages with the same cause. (#1682)
- Handler method, argument, Kotlin suspend-call preparation, and Protobuf parser lookup now occur at registration, reducing per-message reflection and metadata lookup. Also fixed User Spot placement incorrectly rejecting an available target during a readiness transition. (#1657)

## Defect Fixes

- Fixed inconsistent handling of requests with malformed envelopes across languages. An error reply is sent when the header is a syntactically valid JSON object with a valid correlation ID and a reply token; otherwise the failure is recorded without a reply. (#1671)
- Aligned error classification across languages so invalid Spot Configure handler and subscription registrations are rejected as `InvalidOperation` before the activation becomes Ready. (#1715)
- Relocation now uses the new owner generation returned by the Store CAS and carries sealed Session bindings. This fixes paths that used inconsistent generations or omitted binding state when finalizing Session routes after relocation. (#1807)
- Fixed the Framework passing fixed or unlimited HWM values to Core when no HWM was specified. Only explicit overrides are applied; other settings use Core defaults. (#1684)
- Fixed default server and Stream Connector typed JSON accepting numeric enum ordinals, BOMs, noncanonical 64-bit integer strings, or nonfinite numbers. Java and Kotlin use shared `framework-json-v1` validation. (#1703, #1714)
- Fixed shutdown failures or hangs caused by unnecessary Actor unbound after physical Session termination or a receive pump waiting for itself to stop; remote bindings retire on close, and resource-close stages remain ordered even after the deadline. New-binding relay is preserved while the previous Session disconnect callback is pending. (#1708, #1810, #1828)
- Fixed User Spot close and `onClosing` waiting for each other indefinitely with asynchronous Location Stores such as Redis. Close now resumes on the original Spot execution turn after the authority read. (#1727)
- Fixed node startup failing because open generic handler definitions were included in scan candidates. Non-instantiable candidates are skipped, while invalid explicit registrations remain rejected. (#1736)
- Fixed cross-node deferred Join with an empty application reply being rejected as `DataLost`. Empty replies carry no metadata, recovery accepts them, and the target no longer repeats reply-format validation. (#1737, #1746)
- Fixed local requests and Actor sends during movement reporting admission success before owner-queue acceptance. Caller admission continuations run in a new turn separately from internal owner observation, and failures before queue acceptance reach admission and reply with the same cause. (#1759, #1764, #1810)
- Fixed slow status observers or insufficient common-pool workers stalling other observers, topology queries, and lane completion. Observer delivery and infrastructure completion are separated, and topology projections are collected outside the source-map lock. (#1804, #1806, #1810)
- Fixed remote logical multicast stalling at small queue capacities while holding a receive permit and waiting for child permits. The first child takes the receive permit, the same source acquires the rest, and ingress no longer excludes targets already selected by the sender snapshot. (#1805)
- Fixed Session bind confirmation failing on Object Servers without an application Entry type. The internal Entry dispatcher is always installed, published after asynchronous initialization, and initialization failures are reported as startup failures. (#1810)
- Uses Java binding 1.19.1, fixing native Message slots remaining in a terminated virtual thread's pool instead of being reused. (#1811)
- Fixed double release or loss of Spot reply frames, pushes through retired bindings after unbind acknowledgement, and Instance Spot join results completing before Ready publication. (#1827)
- Fixed delayed first owner-lease heartbeats caused by scheduling from initial claim completion. The interval is now measured from claim start. (#1830)
- Fixed `safeToShutdown` queries returning true without publishing the change to observers. Queries and observations now share one status snapshot, and `findSpot` returns the current Spot generation rather than the authority generation. (#1833)
- Fixed repeated source authority reads and a hanging host close when the target shut down just after commit and authority became `Missing`. Source work ends with the expired-owner terminal without attempting Preserve. (#1847)
- Fixed cross-node Join and relocation waiting after frame loss during route replacement by resubmitting durable requests and batches or cutover on a different observed route once ready. Only selected-route handshakes are admitted, the same HELLO snapshot is resent until ADMIT with subsequent changes sent through UPDATE, and automatic discovery avoids duplicate connections to peers with an existing route or manual intent. (#1852, #1874)
- Fixed manually connected Ready peers reporting lifecycle generation 0, causing Spot placement and Kotlin TicTacToe requests to time out. Peer generation, revision, and channel counts now come from the admitted topology descriptor. (#1874)
- Fixed a deferred Join whose admission was removed by a 1-second timer regardless of progress, so the target rejected it with `target rejected canonical relocation: 17` or `35` (`DataLost`). The target attempt that accepts PREPARE now owns the prepared resources and releases them only on its terminal path; a prepared-only entry expires after the registration default timeout. A recovery whose admission is gone is reported as `Unavailable`. (#1729)
- Fixed Instance Spot Close releasing a received frame that was still moving into the queue, so the new incarnation failed the same message with `HANDLER_MISSING for packet ''`. Pending frames are owned by the execution queue. Also fixed a startup rollback that appended a NullPointerException to the original exception when startup failed before the channels were created. (#1745)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.30.1")
    implementation("systems.zlink:zlink-http-client:0.30.1")
}
```

The release tag is [`framework-java/v0.30.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.30.1).
[English](./framework-java-0.30.1.md) | [한국어](./framework-java-0.30.1.ko.md)
