[English](./framework-java-0.17.0.md) | [한국어](./framework-java-0.17.0.ko.md)

# ZLink Java Framework 0.17.0 release notes

Framework 0.17.0 uses binding 1.2.1 and Core 1.2.0. Each framework language is versioned independently.

## Contract change

- Sending through a bound session from an Actor with no binding now ends the same way in all five languages. With no valid binding the call ends as `InvalidOperation`, and that failure surfaces at the **call's terminal** like every other call failure, rather than being thrown where the call is built.
- `boundSession()` used to throw on the spot when there was no binding. The failure now arrives through the `CompletionStage`, so the accessor no longer needs a `try` or `runCatching` around it. The same applies to Kotlin.

## Installation

```kotlin
implementation("systems.zlink:zlink-framework-core:0.17.0")
```

The release tag is [`framework-java/v0.17.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.17.0).
