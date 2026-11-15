[English](./framework-java-0.32.3.md) | [한국어](./framework-java-0.32.3.ko.md)

# ZLink Java·Kotlin Framework 0.32.3 Release Notes

Framework 0.32.3 is a hotfix for two defects in 0.32.2. It keeps Java·Kotlin binding 1.23.0 and Core 1.23.0.

## Bug fixes

- Destroying an Actor that the binding had already closed ended with an exception instead of completing normally. Cleaning up an Actor after its user disconnected left an error. Destroying an already closed Actor now completes normally. (#4465, #4481)
- The stream connector created a new thread for every connection, so the thread count grew with the number of connections. Connections now share execution resources, and cancelling a heartbeat timer costs the same regardless of the number of connections. (#4487, #4493)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.32.3")
    implementation("systems.zlink:zlink-http-client:0.32.3")
}
```

The release tag is [`framework-java/v0.32.3`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.32.3).
