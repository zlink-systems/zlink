[English](./framework-java-0.29.0.md) | [한국어](./framework-java-0.29.0.ko.md)

# ZLink Java·Kotlin Framework 0.29.0 Release Notes

Framework 0.29.0 uses Java·Kotlin binding 1.17.0 and Core 1.17.0. (#1554)

## Contract Changes

- A cold activation for a Missing Instance Spot is submitted to the target through Core request/reply. The target owns reservation and activation. A request that loses the Reserve race ends with one `Unavailable` result; it is not forwarded to another target and does not wait for the winner. If no serving node can provide the requested type, the result is `NotFound`; if an omitted type matches multiple candidates, it is `InvalidOperation`; insufficient capacity returns `Unavailable`. (#1467)
- Constructor dependencies declared by Spot, Actor, and handler registrations are validated from metadata at host startup using optional, collection, and name/qualifier selection rules. Startup fails when a required dependency is missing or ambiguous. Explicit factories are not invoked. Spring selects dependencies with `@Qualifier` and parameter names. (#1549)
- On target restart, recovery releases Creating reservations from the previous lifecycle and returns their pending capacity. (#1532)

## Changes

- Stream nodes now expose `heartbeat` and `idleTimeout` to configure the heartbeat interval and timeout, and the application idle timeout. The heartbeat defaults are 1 second and 5 seconds. The idle timeout now defaults to 0, which disables idle closure. Heartbeat control frames do not count as application activity. Spring applications use the same Java builder settings. (#1538, #1541)
- The Spring starter exposes a public runtime status bean. Applications can query and observe runtime state without depending on the internal lifecycle implementation. (#1550)

## Defect Fixes

- A Store provider failure now returns the public `Unavailable` error from Spot Send and Request operations. The provider cause is preserved. (#1466)
- Core 1.17.0 sends each STREAM message over WS and WSS as one WebSocket message. TCP batching is unchanged. (#1545)
- A Java STREAM monitor wait no longer occupies a virtual thread carrier and blocks `bind()` or state-lane progress. The monitor wait now runs on a platform daemon thread. (#1546)
- Relocation shutdown no longer loses the target terminal when shutdown starts between stage completion and installation. The source sends abort after it restores the queue. (#1490)
- The Instance Spot Store requirement is checked once at host startup. Factories that include the disabled policy follow the same requirement. (#1490)
- The Spring host now propagates runtime startup failure as context startup failure. (#1525)
- SpotWide Actor queues are consumed through the shared Spot execution gate. (#1496)
- The ZoneWorld sample chooses scenarios from the actual owner placement and processes restart notifications in arrival order. (#1490)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.29.0")
    implementation("systems.zlink:zlink-http-client:0.29.0")
}
```

The release tag is [`framework-java/v0.29.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.29.0).
[English](./framework-java-0.29.0.md) | [한국어](./framework-java-0.29.0.ko.md)
