[English](./framework-java-0.34.1.md) | [한국어](./framework-java-0.34.1.ko.md)

# ZLink Java Framework 0.34.1 Release Notes

Framework 0.34.1 is a hotfix for defects in 0.34.0 that blocked the templates. It keeps Java binding 1.26.0 and Core 1.26.0.

## Bug fixes

- Calling `getOrCreate` with the same ID right after a Play replacement ended in `InternalFailure` regardless of the cause, so callers couldn't tell whether a retry was safe. A failed Actor creation now returns the kind of its original cause (for example `Unavailable`). (#5461, #5492)
- When a message failed to serialize as JSON (for example, a field not declared nullable was `null`), the failure reached neither the message-flow log nor stderr. It is now recorded as `zlink.dispatch_error` (`reason=encode_error`), and the error message names the message type and field path and points to `@Nullable`. Routed one-way handler and decode failures are recorded too. (#5454, #5501)
- A planned Host relocation was rejected on the target with "Session routes are not canonical" when one SpotWide held Actor IDs of different lengths (such as `p3-…` and `capacity-p4-…`). Session routes in participant order are now accepted, and only duplicate ActorIds are rejected. When a relocation target fails PREPARE, the failed step and the original exception are recorded. (#5473, #5515, #5504)
- When a client closed its connection right after a `send`, the Session dropped a message it had already accepted instead of relaying it to the bound Actor. Messages accepted before the connection closed are now handed to the Actor before the disconnect notification is sent (up to the lifecycle deadline). (#5483, #5511)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.34.1")
    implementation("systems.zlink:zlink-http-client:0.34.1")
}
```

The release tag is [`framework-java/v0.34.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.34.1).
