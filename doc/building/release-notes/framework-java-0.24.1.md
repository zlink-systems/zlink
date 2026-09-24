[한국어](./framework-java-0.24.1.ko.md) | [English](./framework-java-0.24.1.md)

# ZLink Java·Kotlin Framework 0.24.1 Release Notes

Framework 0.24.1 uses Java·Kotlin binding 1.7.0 and Core 1.7.0.

## Fixes

- Core 1.7.0 fixes the static TLS exhaustion that prevented a host linked statically with libstdc++ from loading `libzlink` through `dlopen()` ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The Java·Kotlin Framework contract and behavior are unchanged.

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.24.1")
    implementation("systems.zlink:zlink-http-client:0.24.1")
}
```

The release tag is [`framework-java/v0.24.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.24.1).
