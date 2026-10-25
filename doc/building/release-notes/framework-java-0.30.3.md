[English](./framework-java-0.30.3.md) | [한국어](./framework-java-0.30.3.ko.md)

# ZLink Java·Kotlin Framework 0.30.3 Release Notes

Framework 0.30.3 uses Java·Kotlin binding 1.20.0 and Core 1.20.0.

## Defect Fixes

- Fixed `GetOrCreate` failing to Join a User Spot while that Spot was closing. (#2672)
- Fixed an exception when calling `Close` with a stale `SpotRef` after the Spot had closed. A second `Close` call joins an in-progress `Close` and receives the same result. (#2675)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.30.3")
    implementation("systems.zlink:zlink-http-client:0.30.3")
}
```

The release tag is [`framework-java/v0.30.3`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.30.3).
