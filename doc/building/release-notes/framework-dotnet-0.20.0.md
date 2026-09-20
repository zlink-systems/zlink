[English](./framework-dotnet-0.20.0.md) | [한국어](./framework-dotnet-0.20.0.ko.md)

# ZLink .NET Framework 0.20.0 Release Notes

Framework 0.20.0 uses binding 1.2.2 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

- The durable-authority-v1 activation recovery pointer is now one five-field shape: `root`, `activationId`, `ownerGeneration`, `replayCursor` and `inboxSequence`. `replayCursor` cannot exceed `inboxSequence`, and the former .NET-specific ZLIS format is not used. (#759)
- The stream connector returns without waiting for connection, disconnect, state, push, error or request-callback handlers to complete. The order in which the disconnect handler runs after `Closed` is unchanged. (#594)
- The target owns completion terminals for remote Actor creation. The requester re-reads the creation terminal with the operation identity after a lost, incomplete or exceptional response; an exception from application creation execution is recorded as a typed `Failed` terminal. Location Store uses one opaque record, `creation-terminal\0{hex(rid)}\0{generation}\0{hex(operationId)}`, whose value is `creation-operation-terminal-v1` schema bytes. (#774)
- The initial owner-lease claim is the first heartbeat operation. A transport failure does not terminate the host; without a lease, startup completes in the §5 blocked state and claims again at each renew interval. `Conflict` and `GenerationExhausted` are typed startup errors. (#790)
- When the durable authority owner lease is `Missing`, the initial claim is treated as a handover; `Found` means another owner is present. .NET and Java storage decisions now match the common behavior already present in C++ and Node. (#682)
- service-wire-v1 uses the schema-dialect rules and generated static codecs. sha256 is `u8 + 32` bytes, ZLIA has a presence byte, `operation-id-or-zero` is an explicit schema case, and reference is `u16`. The .NET codec produces and reads the same bytes as the C++, Java and TypeScript codecs. (#729, #736, #737)
- Remote Actor creation publishes the Framework Entry Spot ID independently of Entry Spot type registration, and reserve, commit and abort are decided from the authority row's single `pendingCreation`. The target owns completion and the terminal; the actor-create-terminal conditional union uses the canonical `u16` body length. (#561, #550, #763)

This is a pre-1.0 contract release. No migration procedure is provided for the existing contract.

## Common Changes

- Added the service-wire lowering path that turns the schema into an operation IR and generates static C++, C#, Java and TypeScript codecs. The generated manifest, fixture catalog, validator and four-language conformance use the same schema. (#736, #780)
- Prepared tutorials and samples to build and run from distribution zips without a repository. The .NET sample runner no longer uses a Python proxy, and the release asset path attaches eight tutorial and sample zips for the four languages. (#655, #639, #673)
- Added the Instance Spot `MatchQueue` example to the four-language tutorials and aligned the C++, .NET, Java, Kotlin and Node/TypeScript snippet markers. (#666)
- Reformatted tutorial and sample sources with each language's formatter and made `scripts/format/format.sh --check` the common entry point. (#761)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.20.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.20.0" />
```

The release tag is [`framework-dotnet/v0.20.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.20.0).
