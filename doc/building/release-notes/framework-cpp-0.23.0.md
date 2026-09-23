[한국어](./framework-cpp-0.23.0.ko.md) | [English](./framework-cpp-0.23.0.md)

# ZLink C++ Framework 0.23.0 Release Notes

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

## C++ Changes

- The three C++ engine adapters (Unreal plugin, Godot GDExtension, Axmol) deliver server pushes subscribed with `Subscribe`/`subscribe(packet_name)` on the engine main thread; previously no push reached the application. A request completion carries the request's packet name (Unreal passed an empty name). (#985)
- The framework-cpp release includes shared prebuilt archives for `linux-x64`, `linux-arm64`, `macos-arm64`, and `windows-x64`. `bootstrap.cmake` downloads the matching archive instead of building the framework from source; consumers do not need Conan or vcpkg for this path. (#855)
- The default C++ build updates the cross-language host to the admission-free Entry Spot contract. (#953)
- The ShoppingMall OrderWorkflow stops requesting relocation readiness after it has already deferred readiness, so the sample can shut down without a forced kill. (#930)

## Install

Choose the `linux-x64`, `linux-arm64`, `macos-arm64`, or `windows-x64` framework archive attached to the [`framework-cpp/v0.23.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.23.0), or run `bootstrap.cmake` to download the matching archive. Use `find_package(zlink_framework CONFIG REQUIRED)`. C++ binding 1.4.0 and Core 1.4.0 are required.

The release tag is [`framework-cpp/v0.23.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.23.0).
