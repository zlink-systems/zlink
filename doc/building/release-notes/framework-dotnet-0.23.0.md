[한국어](./framework-dotnet-0.23.0.ko.md) | [English](./framework-dotnet-0.23.0.md)

# ZLink .NET Framework 0.23.0 Release Notes

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
- The engine examples include one .NET server and Unity and Unreal clients, with smoke and mirror workflows for the integrated sample. (#934, #935)
- The examples mirror is invoked after each language package is published and verified. (#884)
- Core 1.4.0 macOS dylibs use loader-relative install paths, so the release archive is relocatable. (#962)

## .NET Changes

- The .NET samples now include a root `Samples.sln` that contains all sample projects, and the quickstart, tutorial, and samples READMEs explain how to open the solution in an IDE. (#917)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.23.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.23.0" />
```

The release tag is [`framework-dotnet/v0.23.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.23.0).
