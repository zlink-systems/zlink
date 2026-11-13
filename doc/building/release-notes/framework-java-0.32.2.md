[English](./framework-java-0.32.2.md) | [한국어](./framework-java-0.32.2.ko.md)

# ZLink Java·Kotlin Framework 0.32.2 Release Notes

Framework 0.32.2 is a hotfix for one defect in 0.32.1. It keeps Java·Kotlin binding 1.23.0 and Core 1.23.0.

## Bug fixes

- The Spot actor handler list was shared by the whole runtime, and every Spot instance activation wrote to it without synchronization. When the first instances of several Spot types activated at the same time, an activation could fail with `ConcurrentModificationException`, or handlers could be lost so later Actor requests ended with `actor packet handler is not registered`. Each Spot instance now owns its actor handler list, and dispatch reads the list of the target instance. (#4389, #4391)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.32.2")
    implementation("systems.zlink:zlink-http-client:0.32.2")
}
```

The release tag is [`framework-java/v0.32.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.32.2).
