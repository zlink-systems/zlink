[English](./framework-java-0.30.2.md) | [한국어](./framework-java-0.30.2.ko.md)

# ZLink Java·Kotlin Framework 0.30.2 Release Notes

Framework 0.30.2 uses Java·Kotlin binding 1.20.0 and Core 1.20.0.

## Compatibility changes

- `@ZLinkSpotSubscription` now requires `channelName`. A Spot subscription is identified by its channel and topic pair; previously the channel was lost in the catalog and in dispatch. Add the subscribed channel name. The registration API `addSubscribe(handlerType, channelName, topic)` expresses the same subscription. (#1783)

  ```java
  // 0.30.1
  @ZLinkSpotSubscription(topic = "zone.border")
  // 0.30.2
  @ZLinkSpotSubscription(channelName = "zoneworld.zones", topic = "zone.border")
  ```

- Stream Connector `ws://` and `wss://` connections no longer use the JDK default `ProxySelector` and connect directly to the endpoint. A native connector connects directly on every transport (stream-connector spec §3.2). (#2534)

## Changes

- Request admission is distinct from reply completion, so callers can observe acceptance. Deferred Join completion and HTTP handler Yield now continue on the correct execution turn. (#2065, #2044, #1916, #1912)
- Replaced the 1 ms deadline sweep with a deadline-driven worker, removing unnecessary periodic work. (#1820, #1846, #2090, #2094, #2185)
- Kept owner identity, source request ownership, and capacity metric observation consistent during relocation, Session, and Join operations. (#2102, #2151, #2285, #1999, #1912)
- Reduced per-message reads and completion work, and aligned runtime metric labels and lifecycle with the contract. (#1820, #1846, #2090, #2094, #2189)

## Defect Fixes

- Fixed deadlocks and stalls where Join completion, Actor shutdown, or host drain waited on the current execution turn. (#2044, #2122, #2290, #1978, #2248)
- Fixed request and shutdown stalls caused by incorrect Session seal, route replacement, and binding close order during relocation. (#1893, #1913, #1915, #1955, #1921, #1922)
- Fixed startup lease claims and Ready timers being delayed or observing a completed state incorrectly. (#1880, #1897, #2100, #2288)
- Requests without a source descriptor now end as Unavailable, and Session commands use the stored owner identity. (#2140, #2102, #2151)
- The original exception from target Join lifecycle failure is reported through the error reporter. (#2459)
- Updated the Bingo sample so an observer looks up the room state and confirmed rewards after joining. (#2427)
- STREAM connections closed by server drain now report the spec close reason `server_shutdown`. (#2189)
- Fixed a successful startup being judged again as cancelled after stop, which made Location runtime cleanup call unimplemented store operations. (#2466)
- Core 1.20.0 no longer stalls the temporary transport owner's executor when a completion poller is registered during the handshake. (#2504)
- Actor replies are sent as a single STREAM frame, and error payloads use the spec's `{code, message}` form. All four languages verify against the same shared vectors. (#2507)
- Join callbacks run in the Spot lifecycle turn, and defects in turn restoration and teardown owner resolution are fixed. (#2507)
- Fixed the `Continued` callback of a User Spot retire running after a held application job once seal preparation finished. (#2623)
- Fixed observation delivering a status from before the subscription as its first item. The first item is the current status at subscription. (#2629)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.30.2")
    implementation("systems.zlink:zlink-http-client:0.30.2")
}
```

The release tag is [`framework-java/v0.30.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.30.2).
[한국어](./framework-java-0.30.2.ko.md) | [English](./framework-java-0.30.2.md)
