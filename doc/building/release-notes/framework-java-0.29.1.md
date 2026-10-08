[English](./framework-java-0.29.1.md) | [한국어](./framework-java-0.29.1.ko.md)

# ZLink Java·Kotlin Framework 0.29.1 Release Notes

Framework 0.29.1 uses Java·Kotlin binding 1.17.0 and Core 1.17.0.

## Defect Fixes

- Fixed a stall on two-core machines where Java native socket monitor waits occupied virtual-thread carriers and blocked route readiness and request processing. The shared backend owner selects the Stream and Channel monitor executor, and native waits for Stream, Channel, and ClientServer control run on platform threads. (#1567, #1568)
- Fixed the Windows Kotlin GameQuest runner so it creates `close-replay.release` instead of leaving the client and runner waiting for each other. (#1568)
- Fixed a 0.29.0 regression where an operation that arrived while the same Instance Spot was activating failed with `Unavailable` or `stale_target`. A later operation now joins the activation in progress and is processed in arrival order after Ready. (#1571)
- Fixed operations that joined the same target activation being processed out of arrival order under load. (#1571)
- Fixed `Create` and `GetOrCreate` failing with `location owner lease is unavailable` after the owner node of an Actor was killed. For an Actor type whose factory registration disables relocation (`DisableRelocation`), the runtime releases the record whose owner lease has ended and creates a new incarnation. A type with relocation enabled still returns `Unavailable`. (#1570)
- Java `Create` now fails with `AlreadyExists` for an existing Actor instead of returning it, and a stable type mismatch reports `TypeMismatch`, matching the other languages. (#1570)
- Fixed Actor teardown starting dependency cleanup before already accepted handler turns completed. (#1579)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.29.1")
    implementation("systems.zlink:zlink-http-client:0.29.1")
}
```

The release tag is [`framework-java/v0.29.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.29.1).
[English](./framework-java-0.29.1.md) | [한국어](./framework-java-0.29.1.ko.md)
