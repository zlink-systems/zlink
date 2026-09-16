[English](./framework-java-0.16.0.md) | [한국어](./framework-java-0.16.0.ko.md)

# ZLink Java Framework 0.16.0 release notes

Framework 0.16.0 uses binding 1.2.1 and Core 1.2.0. Each framework language is versioned independently.

## Contract change

- A select-one channel left with no member after eligibility and drain now ends as `Unavailable`, and a request and a one-way send agree. A member dropped because its weight is `0` or because it is draining falls here; the send path and the connection are still there, so it is not `NotFound`. The languages used to answer differently.
- Java and Kotlin already ended with this kind, so their behaviour is unchanged.

## Fixes

- Stopping the auto-connect loop now waits for a tick already running before the reconciler shuts down, instead of iterating and mutating the same map at once.
- `zlink-framework-testkit` compiles again; its fake publisher socket did not implement `setNoDrop`.

## Installation

```kotlin
implementation("systems.zlink:zlink-framework-core:0.16.0")
```

The release tag is [`framework-java/v0.16.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.16.0).
