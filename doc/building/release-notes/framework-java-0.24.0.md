[한국어](./framework-java-0.24.0.ko.md) | [English](./framework-java-0.24.0.md)

# ZLink Java·Kotlin Framework 0.24.0 Release Notes

Framework 0.24.0 uses binding 1.6.0 and Core 1.6.0. Each Framework language release is versioned independently.

## Contract Changes

- Fixed RIDs can be used with both manual topology and automatic discovery, regardless of Object role. Setting a fixed RID together with an automatic RID prefix causes a startup configuration error. A restart fails immediately with a conflict while the previous owner lease is active; the same RID can be used after that lease expires. (#1045, #1056)

## Common Changes

- Core 1.5.0 and later fix failures to load `libzlink` with a late `dlopen()` in hosts with limited static TLS space. The Framework uses this Core version through binding 1.6.0. (#1041)
- Core 1.6.0 fixes ROUTER refusing to issue a reply token when the sender disconnects right after a request is received, which ended the receive path with an error (the rare cause of a C++ process abort). (#1051, #1062)
- Java and Kotlin quickstart and tutorial listeners are limited to `127.0.0.1`. The tutorial explains how to remove its keys when reusing Redis and how to restart after a forced stop by waiting for the previous owner lease to expire or removing the relevant keys. (#1035, #1043, #1045, #1047, #1049)

## Fixes

- The Godot C# and C++ examples now place their Windows DLLs, use the corrected C++ build settings, and document execution so they communicate with the engine server in Godot 4.4.1. (#1037, #1040)
- The Unity (Windows native and WebGL) and Cocos Creator examples' manifest, scene, received-value logging, and README were fixed so that they communicate with the engine server in Unity 6000.0.83f1 and Cocos Creator 3.8.8. (#1037, #1064, #1066)
- Java configuration now throws `ZLinkConfigurationException` when a fixed RID and an automatic RID prefix are both specified, regardless of call order. (#1045, #1058)

## Java and Kotlin Changes

- The Java ZoneWorld sample waits for boundary event publication to complete before sending zone state. This fixes intermittently missing boundary events in ZW-B4. (#1054, #1055)
- Corrected the Kotlin tutorial README's mirror repository address to `zlink-kotlin-examples`. (#1035)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.24.0")
    implementation("systems.zlink:zlink-http-client:0.24.0")
}
```

The release tag is [`framework-java/v0.24.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.24.0).
