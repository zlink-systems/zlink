[English](./framework-java-0.34.2.md) | [한국어](./framework-java-0.34.2.ko.md)

# ZLink Java Framework 0.34.2 Release Notes

Framework 0.34.2 is a hotfix for a defect in 0.34.1 that broke the Kotlin board-tables template. It keeps Java binding 1.26.0 and Core 1.26.0.

## Bug fixes

- After an Actor moved to another node (for example, joining a Spot hosted elsewhere), the original node kept a stale copy of that Actor when it had no bound session. Creating or getting the same Actor ID on the original node again — for example, a player reconnecting after the other node stopped — then failed with "actor already exists". The original node now removes its copy when the move commits, a new creation waits until the previous generation has fully left, and late messages for the previous generation follow the Message Follow route instead of reaching the new Actor. (#5573, #5585)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.34.2")
    implementation("systems.zlink:zlink-http-client:0.34.2")
}
```

The release tag is [`framework-java/v0.34.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.34.2).
