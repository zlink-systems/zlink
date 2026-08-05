<!-- framework-adapter-nav:start -->
[Guide Home](../index.en.md) | [Next: ZLink Framework for .NET — Overview](guide/server/01-overview.en.md)
<!-- framework-adapter-nav:end -->

[Guide Home](../index.en.md) | [Common Spec](../common/README.ko.md)

[Common Spec](../common/README.ko.md) | [Async Execution](../common/spec/05-async-execution-policy.ko.md) | [Exact Interface](../common/spec/server/languages/dotnet/interfaces/README.ko.md) | [Stream Connector](../common/spec/stream-connector/languages/dotnet/03-stream-connector.en.md) | [Unity Guide](guide/stream-connector/02-unity.ko.md) | [Common Internals](../common/internals/README.en.md) | [Regression Matrix](internals/regression-test-matrix.en.md) | [Backend Policy](internals/backend-dependency-policy.en.md)

# ZLink Framework for .NET

> **This directory is the entry point for `.NET` documentation.** It puts usage guidance
> for three packages in one place — **contracts split by package, usage guidance split by
> language.**
>
> | Directory | What |
> |---|---|
> | [`guide/server/`](guide/server/01-overview.en.md) | The **framework (server)** usage guide |
> | [`guide/http-client/`](guide/http-client/README.ko.md) | The **HTTP client** usage guide |
> | [`guide/stream-connector/`](guide/stream-connector/README.ko.md) | The **Stream connector** usage guide (including Unity/Godot) |
> | [`internals/`](internals/regression-test-matrix.en.md) | Implementation and verification criteria |
>
> **The public contract doesn't live here.** It's owned by the [spec tree](../common/spec/README.ko.md) —
> [server/languages/dotnet](../common/spec/server/languages/dotnet/README.ko.md),
> [http-client/languages/dotnet](../common/spec/http-client/languages/dotnet/dotnet-http-client.en.md),
> [stream-connector/languages/dotnet](../common/spec/stream-connector/languages/dotnet/03-stream-connector.en.md).
> Where the guide and the contract diverge, **the contract wins.**

## 1. Purpose

This document organizes the `.NET` surface of `ZLink Framework`, which sits on top of the
`.NET` binding. It prioritizes these three axes.

- request/send and event messaging[^channel-messaging] keyed by channel name
- How to work with `SPOT`[^spot] from an `ASP.NET Core` application
- Registering a location store[^location-store] to share peer/spot/actor location and
  query operational status

The framework directly implements Channel, Spot, Actor, and the STREAM service runtime.
From the `.NET` binding, it only uses public raw socket APIs like `DealerSocket` and
`RouterSocket`. It exposes DI, hosted services[^hosted-service], the handler model, and
Location-Store-based auto-connect to the user, while raw socket wiring is handled inside
the Framework.

The current implementation backend uses `bindings/dotnet` as-is. Even so, it's a principle
to keep the public contract the framework shows users separate from the backend
implementation. The exact criteria are covered in
[backend-dependency-policy.ko.md](internals/backend-dependency-policy.en.md).

Sample and E2E config files, the ban on environment variables, and Options-binding
criteria follow the [Sample/E2E Configuration Policy](../common/sample-e2e-configuration-policy.en.md).

## 1.1 Supported Version Baseline

This `.NET` document fixes its version baseline up front, as follows.

- Minimum supported runtime: `.NET 8` (`net8.0`)
- Primary development baseline: `.NET 10` (`net10.0`)
- Minimum supported language version: `C# 12`

So this directory's documentation and samples prioritize explaining a surface that
compiles and runs directly at the minimum supported baseline above. `C# 13`, `C# 14`, and
`preview`/`latest`-only syntax or APIs are not treated as a precondition of the public
framework contract.

## 1.1.1 CI Platform Baseline

This document's CI[^ci] baseline doesn't designate one specific OS as the representative
platform. Instead, the framework side follows the same native runtime range that
`bindings/dotnet/runtimes/` and `.github/workflows/build.yml` in the repository already
manage together.

As of now, the six runtime RIDs[^rid] that must be supported are the following.

- `win-x64`
- `win-arm64`
- `linux-x64`
- `linux-arm64`
- `osx-x64`
- `osx-arm64`

So the `.NET` framework's regression tests and release gate[^release-gate] treat passing
all six platforms above as their baseline condition.

## 1.2 Common Policy Application

Every document in this directory follows the
[Framework Adapter Policy](../common/README.ko.md) and its subordinate documents as-is.
In other words, the `.NET`-specific documents never define new common meaning — they only
cover how already-fixed meaning takes concrete shape on the `.NET`/`ASP.NET Core` surface.

In particular, the following items apply uniformly across this whole directory.

- The naming convention follows the `Naming Policy` in
  [Common Spec README §6.2.1](../common/README.ko.md#621-네이밍-규칙) as-is. In `.NET`,
  the entire public API is written in `PascalCase`, and word composition itself is never
  changed arbitrarily.
- The casing intent behind `Zlink` and `ZLink` is as follows.
  - The native binding package (`bindings/dotnet/src/Zlink/...`) and the raw
    transport[^raw-transport] types it exports (e.g., `DealerSocket`, `RouterSocket`) live
    under the `Zlink` namespace — that is, the wire/transport[^wire-transport] level.
  - Framework adapter surface types are unified under the `ZLink` prefix. For example,
    shapes like `IZLinkSession`, `IZLinkActorContext`, `IZLinkBoundSession`. In other
    words, every interface, record, enum, and exception the framework exposes to the user
    uses `ZLink`.
  - Package ids and namespace words (`Systems.Zlink.*`) follow the native binding
    convention. The casing intent for type names and the casing intent for namespace
    names are separate concerns.
- Packages and namespaces based on the `zlink.systems` domain follow the reverse-domain
  convention[^reverse-dns]. `.NET`'s NuGet[^nuget] package id and namespace use
  `Systems.Zlink.*`. For example, the framework becomes `Systems.Zlink.Framework`, and
  Stream Connector becomes `Systems.Zlink.Stream.Connector`.
- Manual connections are described through per-feature public surfaces, like a MeshNode's
  `PeerConnections` and a fanout subscriber connection. On the same MeshNode,
  location-store-based auto-connect and manual peer connections aren't mixed.
- Send is described as async submit by default. Backpressure[^backpressure] doesn't have
  a separate public no-wait option -- it's handled inside the framework using nonblocking
  send, a pending queue, and ready notification.
- `CancellationToken` is placed only on public async boundaries that can actually wait and
  have that wait cancelled. APIs with a queue, retry, transport write, or reply wait --
  request / actor join / channel submit / SPOT submit / stream connector write -- take a
  token. Conversely, APIs where the current implementation completes immediately or cleans
  up via its own termination token -- session reply frame writing, session close, timer
  cancel -- don't take a token. If an API does take a token, it must be threaded through to
  the actual wait point, not just checked before starting.
- Documents covering `SPOT` must explain stable-type-based factory registration, creation
  and lookup by global `SpotId`, lifecycle timers, and the external spot-publish surface,
  aligned with common policy.
- Spot/Actor messages are sent by global ID. The Framework looks up the current owner's
  NodeRid and generation from the Location Store. Actor join, actor factory registration,
  and the stream-to-actor bridge[^stream-actor-bridge] follow the same location contract.
- Session actor dispatch[^session-actor-dispatch] isn't a single gateway feature switch you
  flip on/off. Instead it's described as a combination of `AddSession<TSession>()` after
  `AddStreamNode`, the actor factory, actor logical actor binding, actor-session binding,
  and `IZLinkBoundSession`. There's no separate public API for session location lookup.

## 2. Document Structure And Division Of Roles

Documents split into four kinds: **guide**, **reference document**, **topic document**,
**sample document**. If this is your first time here, start with the guide. The formal
contract is owned by the reference/topic documents; runnable code by the sample documents.

### 2.0 Guide (Getting Started)

`guide/server/` directly explains concepts and usage so `.NET`/`ASP.NET Core` developers
can **read a feature and immediately write against it.** The formal meaning of a concept is
owned by the common spec, the formal contract by the spec documents, and the guide unpacks
that meaning into real usage code. The business flow of the full runnable samples is
defined by the [common sample](../common/sample/README.en.md).

When writing or editing this guide documentation, follow the
[User Guide Writing Guide](../../../../doc/principal/documentation/guide-writing-guide.ko.md).

Reading order and per-chapter content are laid out by
[the guide's reading order](guide/server/README.en.md).

### 2.1 Reference Document (Interface Catalog)

| Document | Role |
|------|------|
| [interfaces/README.ko.md](../common/spec/server/languages/dotnet/interfaces/README.ko.md) | The formal table of contents splitting the public interface into common runtime, host, channel, Spot, Actor, STREAM, location, maintenance, and monitoring |

### 2.2 Topic Documents (Programming Model)

Each topic document explains the programming model and usage direction. It doesn't
re-enumerate the full interface definitions -- it references the matching category in the
exact interface table of contents.

| Document | Scope |
|------|------------|
| [configuration-host.ko.md](../common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md) | ASP.NET Core host registration, bootstrap, DI, lifecycle, and startup validation |
| [interfaces/README.ko.md](../common/spec/server/languages/dotnet/interfaces/README.ko.md) | The full table of contents for public interfaces/contexts/handlers/clients/providers/observation categories |
| [32-stream-connector.ko.md](../common/spec/stream-connector/languages/dotnet/03-stream-connector.en.md) | The separate client connector's lifecycle, dispatch, codec, transport, and termination reasons |
| [Public contract](../common/spec/server/languages/dotnet/README.ko.md) | The documentation contract and the exact verification procedure against actual assembly/NuGet artifacts |

**The meaning and behavioral rules of a feature are owned by the [common spec](../common/spec/README.ko.md).**
Language-specific documents only fix what shape that meaning takes in `.NET`.

### 2.3 Maintenance Documents

The following documents don't cover public API usage -- they explain backend boundaries,
internal lifecycle, and regression verification. Public errors and allowed combinations
follow each feature's spec.

| Document | Scope |
|------|------------|
| [Common Internals](../common/internals/README.en.md) | Runtime architecture decisions shared across all four languages |
| [regression-test-matrix.ko.md](internals/regression-test-matrix.en.md) | The regression test items that must always be kept, CI tiers, release gate |
| [backend-dependency-policy.ko.md](internals/backend-dependency-policy.en.md) | Backend dependency relationships and the boundary for replacing low-level libraries |
| [public-symbol-delta-v11.ko.md](internals/public-symbol-delta-v11.en.md) | Zero internal migrations and the minimal-public-delta classification for maintenance |

### 2.4 Sample Documents

Sample documents gather runnable code that shows everything at once, from registration
code to handlers to client calls. They don't re-enumerate interface definitions. Feature
selection criteria are owned by the guide; sample documents show the actual
registration/execution flow of the common canonical scenarios.

| Document | Scope |
|------|------------|

### 2.5 Scope Principle

| Concept | Covered here | In other documents |
|------|----------|---------------|
| The full definition of interfaces, attributes, and contexts | [Exact interface](../common/spec/server/languages/dotnet/interfaces/README.ko.md) | Cross-reference only |
| Channel registration (AddZLinkFramework), lifecycle | [Configuration and host](../common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md) | Link only, when needed |
| Handler/client usage examples, dispatch flow | aspnet-core-channel-messaging, samples | |
| SPOT concepts, registration, lifecycle | [Spots](../common/spec/server/languages/dotnet/interfaces/05-spots.en.md) | Link only, when needed |
| Actor lifecycle, session bind, user Spot join, session actor dispatch | [Actors](../common/spec/server/languages/dotnet/interfaces/06-actors.ko.md) | Link only, when needed |
| Location store registration, auto-connect, operational queries | [Location](../common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md) | Link only, when needed |

## 3. Core Direction

- Follows `ASP.NET Core`'s DI and hosted-service model.
- Creation of handlers, clients, and filters is also aligned to the same `.NET DI`
  container.
- A business message addresses its target with a ChannelName, SpotId, or ActorId. The
  Framework decides the current location and send route, so the application never selects
  a specific NodeRid.
- A node-direct call that takes `(MeshName, target NodeRid)` is used only for operations
  where the physical node itself is the target, like Admin/Ops. It's never used for
  Actor/Spot creation or business messages.
- Rather than setting up a separate gateway or dedicated load balancer, it calls directly
  through location-store auto-connect, which uses the MeshNode descriptor.
- A ChannelName messaging handler is registered as a typed handler on that membership's
  builder. An Admin/Ops node-direct handler is registered on the MeshNode builder, so the
  two namespaces don't mix.
- `[ZLinkRequest]`, `[ZLinkSend]`, `[ZLinkPublish]` don't take a channel name as an
  argument. Since the channel name is a value of the deployment environment and topology,
  it's owned by channel registration, not the handler attribute.
- `SPOT` too must be handled inside the framework lifecycle, rather than split off into a
  separate low-level runtime.
- Regular channel messaging has `IZLinkRouteClient` use the process-local send route
  registered under a ChannelName. It picks one of the servers that are Ready and have a
  positive weight. An Admin/Ops node-direct call specifies a MeshName and target NodeRid on
  the same client.
- `SPOT`'s high-level surface covers Logical Multicast publish, send/request keyed by
  global `SpotId`, and actor messaging. The caller never assembles a current owner's
  NodeRid or a separate ingress channel.
- Both `IZLinkRouteClient` and `IZLinkSpotOutbound` take a typed payload, and address
  resolution and wire construction are handled internally by the framework. The caller
  never chooses a raw frame or transport kind.

## 4. Regression Tests

Every detail document in this set must also explain its regression test criteria. So
whenever a document is added or renamed, check that the following tests were updated to
reflect all three of these.

- The document list
- Each document's regression test section
- The link to representative test cases

| Test case | Verification criteria |
|---------------|-----------|
| `RegressionTests.DotNetContractDocuments_AllExposeRegressionTestSection` | Every `.NET` contract/sample/internals document has a "Regression Tests" section. |
| `RegressionTests.DotNetRegressionMatrix_References_AllContractDocuments` | `regression-test-matrix.ko.md` references every document filename currently under verification. |
| `ScaffoldSmokeTests.FrameworkRoot_IsDiscoverable_FromTestRuntime` | The framework root can be found from the test runtime, so the documentation regression tests run against the repository baseline. |

[^public-contract]: A public contract is an API surface exposed to external users, whose compatibility must be maintained when it changes.
[^channel-messaging]: Channel messaging is a way of exchanging messages keyed by a channel name. Request/send refers to request-response and one-way delivery; event messaging refers to publish/subscribe-style event delivery.
[^spot]: `SPOT` is a logical execution unit that's dynamically created and destroyed. Rooms, stages, and zones are representative examples.
[^topology]: Topology is configuration information showing which nodes (channel, spot, location row, etc.) exist where, and how they're connected to each other.
[^location-store]: A location store is a shared store where a server writes its own endpoint, routing id, and ChannelName membership as a descriptor row, and other servers read that row to find a connection target. A production configuration explicitly registers the official Redis extension or an `IZLinkLocationStore` implementation.
[^hosted-service]: A hosted service is a background component that starts and stops together with the `ASP.NET Core` host (`IHostedService`).
[^ci]: CI (Continuous Integration) refers to a pipeline that automatically builds and tests on every incoming code change, catching regressions quickly.
[^rid]: An RID (Runtime Identifier) is a string `.NET` uses to identify an OS/CPU combination. E.g., `win-x64`, `linux-arm64`.
[^release-gate]: A release gate is the bundle of verification steps (tests, build, checks) that must pass before a new version is deployed.
[^raw-transport]: Raw transport refers to send/receive at the low-level socket layer, without going through the framework's abstraction.
[^wire-transport]: The wire/transport level refers to the layer where bytes actually flow over the network, on top of which the framework's abstraction is built.
[^reverse-dns]: The reverse-DNS convention is the practice of flipping a domain name backward to avoid namespace collisions. For the `zlink.systems` domain, that becomes `Systems.Zlink.*`.
[^nuget]: NuGet is `.NET`'s standard package manager, distributing and installing libraries as package ids.
[^capability]: A **role** refers to a unit of functionality (e.g., server, subscriber, publisher) that a node (channel, spot, etc.) exposes externally.
[^backpressure]: Backpressure is a mechanism that regulates flow so the sending side can't push messages in faster than the receiving side can process them.
[^stream-actor-bridge]: The stream-to-actor bridge is the connection point that carries external traffic arriving over STREAM into the framework's internal actor messages.
[^session-actor-dispatch]: Session actor dispatch is the pattern of automatically forwarding a request arriving from a client session to the actor bound to that session.
[^fail-fast]: Fail-fast is a strategy that throws an exception and halts execution immediately upon detecting an invalid configuration or state, preventing it from being caught late and growing into a bigger problem.
[^attribute-scan]: An attribute scan is a way of scanning the types and methods defined in an assembly to find and register items carrying a specific attribute.

---
<!-- framework-adapter-nav:bottom:start -->
[Guide Home](../index.en.md) | [Next: ZLink Framework for .NET — Overview](guide/server/01-overview.en.md)
<!-- framework-adapter-nav:bottom:end -->
