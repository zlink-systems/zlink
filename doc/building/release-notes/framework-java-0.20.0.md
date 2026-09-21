[English](./framework-java-0.20.0.md) | [한국어](./framework-java-0.20.0.ko.md)

# ZLink Java·Kotlin Framework 0.20.0 Release Notes

Framework 0.20.0 uses binding 1.2.1 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

- MeshNode `receiveTimeout` is applied as the ROUTER socket receive timeout. (#606)
- The durable-authority-v1 activation recovery pointer is now one five-field shape: `root`, `activationId`, `ownerGeneration`, `replayCursor` and `inboxSequence`. `replayCursor` cannot exceed `inboxSequence`, and the former four-field format is not used. (#759)
- The stream connector returns without waiting for connection, disconnect, state, push, error or request-callback handlers to complete. Java already followed this common rule; the other language implementations now match it. (#594)
- The target owns completion terminals for remote Actor creation. The requester re-reads the creation terminal with the operation identity after a lost, incomplete or exceptional response; an exception from application creation execution is recorded as a typed `Failed` terminal. Location Store uses one opaque record, `creation-terminal\0{hex(rid)}\0{generation}\0{hex(operationId)}`, whose value is `creation-operation-terminal-v1` schema bytes. (#774)
- The initial owner-lease claim is the first heartbeat operation. A transport failure does not terminate the host; without a lease, startup completes in the §5 blocked state and claims again at each renew interval. `Conflict` and `GenerationExhausted` are typed startup errors. (#790)
- When the durable authority owner lease is `Missing`, the initial claim is treated as a handover; `Found` means another owner is present. Java's storage decision now matches the common behavior in .NET, C++ and Node. (#682)
- service-wire-v1 uses the schema-dialect rules and generated static codecs. sha256 is `u8 + 32` bytes, ZLIA has a presence byte, `operation-id-or-zero` is an explicit schema case, and reference is `u16`. The Java codec produces and reads the same bytes as the C++, C# and TypeScript codecs. (#729, #736, #737)
- Remote Actor creation publishes the Framework Entry Spot ID independently of Entry Spot type registration, and reserve, commit and abort are decided from the authority row's single `pendingCreation`. The target owns completion and the terminal; the actor-create-terminal conditional union uses the canonical `u16` body length. (#561, #550, #763)

This is a pre-1.0 contract release. No migration procedure is provided for the existing contract.

## Common Changes

- Added the service-wire lowering path that turns the schema into an operation IR and generates static C++, C#, Java and TypeScript codecs. The generated manifest, fixture catalog, validator and four-language conformance use the same schema. (#736, #780)
- Prepared quickstart, tutorials and samples to build and run without a repository. The Java and Kotlin tutorial and sample procedures are included. They are obtained with `git clone https://github.com/zlink-systems/zlink-java-examples` (tag `v0.20.0` matches this release); zip distribution is gone. (#655, #639, #673, #831)
- Added the .NET-equivalent Instance Spot `MatchQueue` example to the Java, Kotlin and Node/TypeScript tutorials and aligned the four-language snippet markers. (#666)
- Reformatted tutorial and sample sources with each language's formatter and made `scripts/format/format.sh --check` the common entry point. (#761)
- Changed Java receive-queue draining from recursion to iteration so long runs of synchronous handlers do not overflow the stack. Added `closeReason()`, unnamed `expectNone()` and `waitForSequence()` to the Kotlin wrapper. The same-node Join barrier remains ahead of the Actor when the dispatch target changes. (#604, #600, #644)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.20.0")
    implementation("systems.zlink:zlink-http-client:0.20.0")
}
```

The release tag is [`framework-java/v0.20.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.20.0).
