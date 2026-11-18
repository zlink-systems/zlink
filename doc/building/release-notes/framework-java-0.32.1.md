[English](./framework-java-0.32.1.md) | [한국어](./framework-java-0.32.1.ko.md)

# ZLink Java·Kotlin Framework 0.32.1 Release Notes

Framework 0.32.1 is a hotfix for one defect in 0.32.0. It keeps Java·Kotlin binding 1.23.0 and Core 1.23.0.

## Bug fixes

- Instance Spot cold activation, new Actor placement and ClientServer candidate selection did not check the owner lease in the Location Store, so they could pick a host that had been killed. Candidates now come only from descriptors whose lease is live. (#4205, #4220)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.32.1")
    implementation("systems.zlink:zlink-http-client:0.32.1")
}
```

The release tag is [`framework-java/v0.32.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.32.1).
