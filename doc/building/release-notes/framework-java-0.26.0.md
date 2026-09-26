[한국어](./framework-java-0.26.0.ko.md) | [English](./framework-java-0.26.0.md)

# ZLink Java and Kotlin Framework 0.26.0 Release Notes

Framework 0.26.0 uses Java and Kotlin binding 1.10.0 and Core 1.10.0.

## Changes

- The Stream Connector closes a connection with reason `ProtocolError` after frame decode failure or an inbound payload limit violation. Under §6.3, it validates every option before connecting: out-of-range values return `ValidationFailed`, and incompatible options return `ConfigurationError`. Closing discards unwritten frames and fails pending requests.
- Spot context `Close` returns a result. Completion is reported after authority is released. This preserves the result-returning contract established on main after 0.25.0.
- Relocation `Restore` is decided only by the source. The wire schema no longer includes `remainingDeadlineMs`; compatibility with pre-1.0 wire formats is not provided. Store retention is rounded up when converted to milliseconds.
- Listener status returns the endpoint confirmed by bind. Placement counts use MeshNode activation records, and Session `actor_slot` is resolved at the start of the turn.

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.26.0")
    implementation("systems.zlink:zlink-http-client:0.26.0")
}
```

The release tag is [`framework-java/v0.26.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.26.0).
