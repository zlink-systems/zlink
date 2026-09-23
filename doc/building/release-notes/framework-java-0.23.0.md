[한국어](./framework-java-0.23.0.ko.md) | [English](./framework-java-0.23.0.md)

# ZLink Java/Kotlin Framework 0.23.0 Release Notes

Framework 0.23.0 uses binding 1.4.0 and Core 1.4.0. Each Framework language release is versioned independently.

## Contract Changes

- STREAM packets can identify an Actor. The header uses `actor_slot` with flag `0x20`; `$zlink.actor.bound` and `$zlink.actor.unbound` are control packets; dispatch context carries the bound Actor; and connectors expose Actor handles through `actors`, `actor(id)`, and `onActorBound`/`onActorUnbound`. Applications that use one Actor are unchanged. (#933)
- Logical Multicast records each routed target submission failure as `dispatch_error` with the target RID, topic, and a `stale_target`, `backpressure`, or `shutdown` reason. The publish terminal result is unchanged. (#928)
- The four Deferred Actor Join-only limits (64 operations per handler, 8 MiB total per request set, 1 MiB per request, 1 MiB per reply) are removed. Cross-node Join requests and application replies follow the service wire's application payload size rule. (#925)
- Dispatch failures record `error_type` and `error_message` in both message-flow traces and structured logs. (#929)
- Entry Spot joins no longer require an admission callback, and an unselected object role is `None`. Channel target classification and message-size diagnostics now follow the aligned Framework defaults. (#922)

## Common Changes

- Guide code examples are executed snippets taken from tutorial or sample sources, and long chapters are split into smaller sections. (#943)
- Guides now identify the tutorial code behind each example, explain its execution result, describe `set_advertise_host` and `InMesh`, and document STREAM client connectors. (#903, #904, #905, #920)
- Example READMEs show whether each block runs in bash or PowerShell, keep verification in one place, document the stop procedure, and use formal Korean prose. (#890, #891)
- Java and Kotlin examples are exported to separate read-only mirrors. (#894)
- The engine examples include one .NET server and Unity, Unreal, Godot (C# and C++), Axmol and Cocos Creator (web) clients. Every client follows the same engine-lobby contract and ships from its engine's mirror repository (`zlink-engine-server`, `zlink-<engine>-examples`); the guide site has a game engine integration chapter. (#935, #980, #982, #983, #984, #987)
- The examples mirror is invoked after each language package is published and verified. (#884)
- Core 1.4.0 macOS dylibs use loader-relative install paths, so the release archive is relocatable. (#962)

## Java/Kotlin Changes

- The Kotlin ZoneWorld sample's border subscription handlers declare their topics, so the sample passes its start check. (#999)
- The DeliveryDispatch Kotlin sample no longer contains the obsolete Registry module, so the sample builds with the current registry API. (#959)
- The Bingo sample preserves the reward announcement after the report reply under concurrent preparation, and the Java runtime cleans up a STREAM connection when the standard disconnect notification arrives. (#916, #915)
- RouteMesh ingress preserves the wire `flow_id`/`flow_origin` pair in Java message-flow records. (#923)
- Kotlin provides reified extensions for the registration surfaces that previously required Java `Class` arguments. Kotlin examples use wrapper terminators, and `ForbiddenMethodCall` checks reject Java terminal forms such as `submit(Class)` and `submit().await()`. (#911, #895, #896)
- The Java sample runner no longer fails under Bash 5.2 because its standalone Gradle exit trap does not refer to a released local variable. The ZoneWorld shutdown drain and the Windows tutorial manual-peer path are also corrected. (#906, #899, #887)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.23.0")
    implementation("systems.zlink:zlink-http-client:0.23.0")
}
```

The release tag is [`framework-java/v0.23.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.23.0).
