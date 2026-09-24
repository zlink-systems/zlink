[한국어](./framework-java-0.25.0.ko.md) | [English](./framework-java-0.25.0.md)

# ZLink Java·Kotlin Framework 0.25.0 Release Notes

Framework 0.25.0 uses Java·Kotlin binding 1.7.0 and Core 1.7.0.

## Fixes

- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The samples README scopes its IntelliJ IDEA instructions to the source repository ([#1035](https://github.com/zlink-systems/zlink/issues/1035)).

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.25.0")
    implementation("systems.zlink:zlink-http-client:0.25.0")
}
```

The release tag is [`framework-java/v0.25.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.25.0).
