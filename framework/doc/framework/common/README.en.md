<!-- framework-adapter-nav:start -->
[Guide Home](../index.en.md) | [Previous: ZLink Framework](../index.en.md) | [Next: ZLink Framework Overview](spec/02-overview.en.md)
<!-- framework-adapter-nav:end -->

[Guide Home](../index.en.md)

[Spec Table Of Contents](spec/README.en.md) | [Public Contract Governance](spec/00-public-contract-governance.en.md) | [Common Internals](internals/README.en.md) | [Overview](spec/02-overview.en.md) | [Interaction Model](spec/03-interaction-model.en.md) | [Message Model](spec/04-message-model.en.md) | [Channel Topology](spec/07-channel-topology.en.md) | [Framework API](spec/06-framework-api.en.md) | [Async Execution](spec/05-async-execution-policy.en.md) | [Actor Model](spec/14-actor-model.en.md) | [Spot Actor Join / Relocation](spec/15-spot-actor.en.md) | [Session Actor Dispatch Usability](spec/20-session-actor-dispatch.en.md) | [Message Flow Tracing](spec/26-message-flow-tracing.en.md) | [Location Runtime](spec/21-location-runtime.en.md) | [Redis Store](spec/22-location-store-redis.en.md) | [Spot Address Messaging](spec/16-spot-address-messaging.en.md) | [Per-Language Public Contract](spec/server/languages/README.en.md) | [Sample/E2E Configuration Policy](sample-e2e-configuration-policy.en.md) | [Common Sample](sample/README.en.md) | [Scenario E2E](e2e/README.en.md) | [Performance Testing](perf/README.en.md)

# ZLink Framework Common Spec

> This document set is the language-neutral **common spec.** The meaning defined here is
> never redefined by a language-specific document — each language surface only makes it
> concrete.

## 1. Purpose

This set organizes the direction of `ZLink Framework` for developers using `ASP.NET
Core`, `Spring Boot`, `NestJS`, or the C++ zlink framework host on top of zlink's
`.NET`, `Java`, `Kotlin`, `Node.js`, and `C++` bindings. See
[01-overview.ko.md](spec/02-overview.en.md) for the product overview and core value.

## 2. Version Baseline

Supported languages and runtime versions must be specified first by the binding
documents. In particular, the `.NET` documents apply the following baseline uniformly.

- The minimum supported runtime is `.NET 8` (`net8.0`).
- The primary development baseline is `.NET 10` (`net10.0`).
- The minimum supported C# language version is `C# 12`.
- Documentation and samples never assume `preview`, `latest`, or `C# 13`/`C# 14`-only
  syntax that doesn't hold at the minimum supported version.

Even if the binding implementation and samples are also developed on a higher runtime, the
public framework contract must first state clearly "how far down does minimum support
go."

## 3. Document Structure

Each document below covers exactly one topic, organized so their scopes don't overlap.
Reading them in numeric order naturally builds the full picture.

| Order | Document | Scope |
|:----:|------|------------|
| 1 | [01-overview.ko.md](spec/02-overview.en.md) | Product overview, core differentiators, current priority scope. Answers "what is ZLink Framework, and why is it needed." |
| 2 | [02-interaction-model.ko.md](spec/03-interaction-model.en.md) | Defines the meaning of the request-response, command, publish-subscribe, and stream models visible to the user. |
| 3 | [03-message-model.ko.md](spec/04-message-model.en.md) | Covers the server-to-server multipart `header + payload` message structure, the STREAM single-packet boundary, header fields, payload codec direction, and codec extension policy. |
| 4 | [10-channel-topology.ko.md](spec/07-channel-topology.en.md) | Covers channel grouping, discovery, manual connection, the interaction model, and internal transport mapping. |
| 5 | [05-framework-api.ko.md](spec/06-framework-api.en.md) | Covers the API surface direction based on `ASP.NET Core`, `Spring Boot`, `NestJS`, and the `C++` standalone host. |
| 6 | [Async Execution And Coroutine Policy](spec/05-async-execution-policy.en.md) | Defines the common meaning of async submit, the ban on blocking alternatives, and coroutines/adapters. |
| 7 | [22-actor-model.ko.md](spec/14-actor-model.en.md) | Defines the actor lifecycle, session bind, user Spot join, outbound actor calls, and registration surface. |
| 8 | [Spot Actor Join / Relocation](spec/15-spot-actor.en.md) | Defines the admission, commit, callback order, and failure handling when an actor moves between the Entry Spot and a user Spot. |
| 9 | [Session Actor Dispatch](spec/20-session-actor-dispatch.en.md) | Defines the typed handler, route resolver, helper, `SessionProxy`, and error meaning of session actor dispatch. |
| 10 | [Message Flow Tracing And Dispatch Observation](spec/26-message-flow-tracing.en.md) | Defines the modes, events, observer, performance, runtime toggle, and correlation contract of message flow tracing. |
| 11 | [40-location-runtime.ko.md](spec/21-location-runtime.en.md) | Defines peer/spot/actor/route location, owner lease, store/resolver, auto-connect, and the operational query contract. |
| 12 | [41-location-store-redis.ko.md](spec/22-location-store-redis.en.md) | Defines the official Redis location store extension's keys, lease, atomic write, error, and test contract. |
| 13 | [24-spot-address-messaging.ko.md](spec/16-spot-address-messaging.en.md) | Defines spot/actor target addressing, lookup and re-lookup, failure classification, and the relocation boundary. |
| 14 | [Sample/E2E Configuration Policy](sample-e2e-configuration-policy.en.md) | Defines config files, the ban on environment variables, per-language typed binding, and runner responsibility. |
| 15 | [Common Sample Scenarios](sample/README.en.md) | Defines the language-neutral business flow, server roles, messages, and verification criteria for the 6 canonical samples. |
| 16 | [Scenario E2E Tests](e2e/README.en.md) | Verifies scale-out, failure, lifecycle, and observability combinations against a real multi-process structure. |
| 17 | [Performance Testing](perf/README.en.md) | Defines the specification for measuring performance under the same conditions and metrics across every framework language. |
| 18 | [.NET Documentation](../dotnet/README.en.md) | The `.NET`/`ASP.NET Core`-specific documentation entry point. |
| 19 | [Java Documentation](../java/README.en.md) | The `Java`/`Kotlin`/`Spring Boot`-specific documentation entry point. |
| 20 | [Node.js Documentation](../node/README.en.md) | The `Node.js`/`NestJS`-specific documentation entry point. |
| 21 | [C++ Documentation](../cpp/README.en.md) | The `C++` zlink framework host-specific documentation entry point. |

Get the full picture from the overview (1), then read the interaction and message models
(2-3), topology (4), API and async execution (5-6), the actor and Spot contract (7-9), and
observation and location management (10-13) in order. Then check the configuration policy
(14) for how sample/E2E config is passed. Check the canonical business flow with the
common sample (15), the verification criteria with E2E (16) and performance (17), then
drop down into the per-language details (18-21).

When reading a per-language detail document for the first time, use this order by
default.

1. Understand the meaning first, from the common documents.
2. Enter through that language's `README.ko.md`.
3. Read that language's interface reference document, topic documents, and sample
   documents in order.

## 4. Scope Principle Per Document

Follow these principles so each document's content doesn't overlap.

| Concept | Covered here | In other documents |
|------|----------|---------------|
| Product definition, differentiators, transport axes, priority scope | overview | Link to overview when needed |
| Interaction model classification and per-model meaning | interaction-model | Link to interaction-model when needed |
| Message structure, header fields, codec | message-model | Link to message-model when needed |
| Channel grouping, discovery, internal mapping | channel-topology | Link to channel-topology when needed |
| Per-framework API surface, DI, handler registration | framework-api, dotnet/ | Link to the matching document when needed |
| Actor concepts, lifecycle, session bind | actor-model | Link to actor-model when needed |
| Spot actor join/relocation completion conditions and callback order | spot-actor | Link to spot-actor when needed |
| Session actor dispatch | session-actor-dispatch | Link to session-actor-dispatch when needed |

## 5. Documentation Writing Principles

- If a new common behavior is needed, first check whether a contract basis already exists
  in the common spec.
- The framework's target public contract is fixed in the formal spec and the per-language
  exact interface before it's implemented. The gap against the current implementation is
  tracked only in `90-implementation-gap.ko.md`.
- A finished business flow goes into `sample/`; implementation verification requirements
  go into `e2e/`.

This document set follows the approach of "writing the purpose first, then narrowing the
API to fit that purpose" — not "writing the API first, then attaching a purpose to it
later."

## 6. Writing Rules For Per-Language Detail Documents

This common set isn't documentation for `.NET` alone. The `Java`, `Kotlin`, `Node.js`, and
`C++` detail documents must also be writable at the same level, based on this set.

So each per-language directory follows the rules below.

### 6.1 Never Redefine A Common Document

A language-specific document must never newly define the following meaning.

- The name and meaning of an interaction model
- The common meaning of a message header
- The base relationship between channel grouping and discovery/manual connection
- The canonical sample scenarios and E2E verification criteria

To change this meaning, the common document must be changed first.

### 6.2 A Language-Specific Document Gives Common Concepts Concrete Shape

A language-specific document must write the following down in that language's idiom.

- Registration API names
- Client/publisher/manager interface names
- Language-specific surfaces like context, options, attribute/decorator/interface
- The DI or lifecycle integration approach
- What the actual call looks like in sample code

Where the common document fixes "what meaning something must carry," the language
document writes "what signature and sample that meaning takes in this language."

### 6.2.1 Naming Convention

The public naming convention for framework documents follows the `Naming Policy` in
[bindings/doc/spec/README.en.md](../../../../bindings/doc/spec/README.en.md) as-is. This
common document and the per-language detail documents must together keep the following
rules.

- Keep concept names as identical as possible across bindings.
- The only variation allowed in an actual language document is per-language casing
  difference and the minimal suffix needed in a language with no overloading.
- Word substitution, word omission, and adding a separate name with the same meaning are
  not allowed.
- Don't lengthen a name just because the parameter combination differs.
- The common meaning of async submit, the ban on blocking alternatives, and coroutine
  adapters follows the
  [Async Execution And Coroutine Policy](spec/05-async-execution-policy.en.md).
- A builder terminator's name keeps its common meaning but is projected to match each
  language's fluent-API convention. For example, the `.NET` awaitable terminator is
  `Async(...)`, Java's is `submit(...)` / `await(...)`, the C++ coroutine terminator is
  `async()`, and the Node.js `Promise` terminator is `submit(...)`.

The per-language casing to follow first in documents is as follows.

- Java: methods in `camelCase`, classes and annotations in `PascalCase`
- C#: the entire public API in `PascalCase`
- Kotlin: methods in `camelCase`, classes and annotations in `PascalCase`
- C++: methods in `snake_case`, types with a `_t` suffix
- Node/TypeScript: methods in `camelCase`, classes in `PascalCase`

Framework adapter documents also must not use names like `sendWithRoutingId`,
`request_callback`, `publishToTopic`, `recvTimeout` — keep the canonical action name
wherever possible. For example, align as follows.

- `Send`, `Request`, `Publish`
- `SendToNode`, `RequestToNode`
- `SendToChannel`, `RequestToChannel`
- `Connect`, `Bind`, `Close`
- `CreateAsync`, `GetAsync`, `ListAsync`

In summary:

- The common document decides a name's meaning and canonical word composition.
- A language document only converts that name to match that language's casing rules.
- Sample code and body prose must follow the same rule too.

### 6.3 The Minimum Document Set For A Per-Language Directory

Building `.NET`-level detail documentation for a new language requires at least the
following document set.

| Document kind | Role |
|----------|------|
| `README.ko.md` | The entry point for that language's set. Organizes document structure, division of roles, and core direction. |
| Interface reference document | Gathers the common interface / context / configuration surface / attribute or decorator in one place. The boundary between the common contract and internal runtime implementation follows [05-framework-api.ko.md §1.1](spec/06-framework-api.en.md#11-the-boundary-between-public-contract-and-runtime-implementation). |
| Channel messaging topic document | Explains channel registration, the handler model, the outbound client, and the dispatch flow. |
| Channel messaging sample document | Provides a sample showing everything at once, from registration to handler to client call. |
| `SPOT` topic document | If that language supports `SPOT`, explains lifecycle, publish/subscribe, and RouteMesh registration. |
| `SPOT` sample document | Shows a real flow like room/stage/zone in code. |
| Actor / Entry Spot topic document | Explains the actor factory, Entry Spot registry, user Spot registry, actor packet handler, and join/leave lifecycle handler. |
| Actor / Entry Spot sample document | Shows, in one example, the flow of handling authentication or target-Spot selection at the Entry Spot and handling a domain packet at the user Spot. |
| `STREAM` topic document | Explains framework-Header-based packet sessions and their public contract. |
| `STREAM` sample document | Shows registration and handler code at once. |
| Monitoring topic document | Explains socket/discovery/registry/spot runtime events and the registration model. |
| Registry topic document | Explains embedded/standalone, the query surface, and topology queries. |

Even if some axis isn't yet implemented for language reasons, the target contract is never
dropped from the formal spec. The gap against the current implementation and follow-up
plans are tracked only in `90-implementation-gap.ko.md`.

### 6.3.1 Representative Framework Baseline

A per-language detail document is written first against the representative framework or
host below.

| Language | Representative baseline |
|------|-----------|
| `.NET` | `ASP.NET Core` |
| `Java` | `Spring Boot` |
| `Kotlin` | `Spring Boot` |
| `Node.js` | `NestJS` |
| `C++` | The zlink framework host |

`C++` is explained not as an adapter on top of an existing web framework like the other
languages, but as the zlink framework host directly owning the lifecycle and dispatch
loop. Even in this case, there's no public contract for the application to start a runtime
implementation directly. Each per-language detail document must separate the public
contract the application accesses from the adapter's internal runtime implementation. Even
if a runtime implementation type remains in tests or inside the adapter, the user guide
and the package/module entry point never expose a path to directly create or start a
runtime.

### 6.4 The Minimum Checklist For A Per-Language Document

A per-language detail document must clearly write down the following items.

- How is a local channel registered
- How is an outbound channel registered
- How are auto-connect and manual connection configured
- How is the default packet key resolved on a request/send/event call
- What `options` or equivalent structure holds variations like timeout, packet override
- How is the internal submit policy for send/publish and `SendTimeout`-based backpressure
  handling explained
- What ingress is handler dispatch explained against
- What path handles receiving an outbound reply
- Is an outbound-only app possible
- If `STREAM` is supported, does it explain whether the Framework's internal recv loop
  hands a packet to a managed queue before running the session callback, and whether the
  same session's callback serialization is guaranteed
- If the actor/session model is supported, does it explain whether an actor callback runs
  in that Actor's serial execution context after the actor attaches to a `Spot`
- If the actor/session model is supported, does it explain the Entry Spot public surface
  in a separate section
- Is there an API and example for registering an actor packet handler on the Entry Spot
- Is there an API and example for registering an actor packet handler on a user Spot
- Does it explain the argument difference between the Entry Spot's and a user Spot's actor
  packet handler
- Does it explain the actor join/leave lifecycle as the Spot member callback
  corresponding to `OnJoinedActor`/`OnLeaveActor`
- Does it explain that the Entry Spot registry and a user Spot registry are separate
  namespaces, so the same actor type and packet name can map differently
- Does it explain that registering an actor packet handler twice for the same actor type
  and packet name within the same registry is a startup validation error
- Does the actor/session model's regression test verify a packet right after join, a
  packet right after a spot move, and a stale session packet separately
- Does the stream session regression test verify callback task dispatch, the same
  session's callback serialization, and that only the enqueue entry point is allowed
- If `SPOT` is supported, how does it explain Spot-type-based factory registration,
  `RoutingId`-based creation and lookup, lifecycle timers, and the external spot-publish
  surface
- If monitoring is supported, what typed events and registration surface does it explain
  socket/discovery/registry/spot runtime events through
- Does the sample code match the actual registration API and interface names

If this checklist isn't satisfied, the common concept is considered not yet sufficiently
brought down to the language surface.

### 6.5 The Rule For Handling The Gap Between A Per-Language Target Contract And Its Implementation

The framework's target public contract is fixed first in the common spec and the
per-language exact interface, even before it's implemented. The contract is never reduced
to the current languages' lowest common denominator just because it isn't implemented yet.
The gap between the current implementation and the target contract, the reason for any
omission, and follow-up plans are tracked only in `90-implementation-gap.ko.md`.

A new public API candidate with no basis in the common spec or guide isn't added directly
to the formal contract — it's reviewed in a separate draft first. Once approved as a
contract, the formal spec and every language's exact interface are updated first, then the
implementation and contract tests are aligned to match. A design that changes even the
Core public contract follows the writing rules of the root `doc/spec/draft/`.

## 7. Per-Language Public Contract

The per-language documents below define exactly what shape the framework server
package's common behavior takes in each language's public API. The signatures recorded
here are the formal contract that language's implementation and contract tests must
follow.

| Language | Public contract |
|------|-----------|
| `.NET` | [dotnet](spec/server/languages/dotnet/README.en.md) |
| Java | [java](spec/server/languages/java/README.en.md) |
| Kotlin | [kotlin](spec/server/languages/kotlin/README.en.md) |
| Node.js framework | [node](spec/server/languages/node/README.en.md) |
| C++ | [cpp](spec/server/languages/cpp/README.en.md) |

The client package's public interface isn't defined here. Stream connector is owned by the
[per-language Stream connector contract](spec/stream-connector/README.en.md), and HTTP
client by the [per-language HTTP client contract](spec/http-client/README.en.md).

The per-language specs aren't documents that copy each other's signatures. They fix the
same common behavior as a public contract that user of that language can use naturally.
The procedure for changing a contract follows
[Public Contract Governance](spec/00-public-contract-governance.en.md).

---
<!-- framework-adapter-nav:bottom:start -->
[Guide Home](../index.en.md) | [Previous: ZLink Framework](../index.en.md) | [Next: ZLink Framework Overview](spec/02-overview.en.md)
<!-- framework-adapter-nav:bottom:end -->
