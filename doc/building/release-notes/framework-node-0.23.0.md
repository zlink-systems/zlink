[한국어](./framework-node-0.23.0.ko.md) | [English](./framework-node-0.23.0.md)

# ZLink Node.js Framework 0.23.0 Release Notes

Framework 0.23.0 uses binding 1.4.0 and Core 1.4.0. Each Framework language release is versioned independently.

## Contract Changes

- A Spot subscription handler with a missing or empty topic stops the host from starting on every registration path (explicit registration or package scan), and the error names the handler; previously the package scan skipped such a handler silently. (#1004)
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

## Node.js Changes

- The public NestJS fanout channel builder now provides the contract-required `setNoDrop` and `subscribe` methods. (#958)
- Send and request use one channel-target classifier, so selection failures have the same result semantics across both operations. (#922)

## Install

```bash
npm install @zlink-systems/framework@0.23.0 @zlink-systems/http-client@0.23.0
```

The release tag is [`framework-node/v0.23.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.23.0).
