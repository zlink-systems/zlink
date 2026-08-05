---
title: "Framework Public Contract Governance"
---

# Framework Public Contract Governance

[Spec table of contents](README.en.md) · [Next: Framework Messaging Glossary](01-glossary.en.md)

> **What this chapter defines** — the ownership and verification rules for
> the Framework public contract.


## 1. Purpose

This document defines the ownership and verification rules for the ZLink
Framework public contract. The public contract includes not just the
types and operations the user can call, but also timeout, cancellation,
error, callback order, ownership, and completion conditions.

## 2. Contract Ownership

The public contract is split into common semantics and language-specific
representation.

| Location | Contract owned |
|---|---|
| This directory and the common spec per package | Language-independent features, states, completion conditions, error semantics |
| A package's `languages/<lang>/` | Actual public types, method signatures, generic/nullable rules, language-specific async representation |
| The Core formal spec | context, message, raw socket, transport, poller, and generic monitoring contracts |
| Framework internals | Wiring for the per-language service runtime, state machines, protocol handling, and thread/executor structure |

The common spec doesn't take any one language's syntax as the standard.
Each language expresses the same feature and observable result in its own
idiom. The exact signature of .NET RouteMesh/MeshNode is owned by
[.NET RouteMesh/MeshNode Interface](server/languages/dotnet/interfaces/03-configuration-topology.en.md).

### 2.1 Production Source Owner

Each deployment package has exactly one production source owner for the
interfaces, calls, contexts, options, results, and errors the application
compiles against. Source directory names follow each language's
convention, but every language keeps the following direction.

- The application contract source is owned by that deployment package.
- Server, HTTP Client, and Stream Connector are each independent
  deployment packages, so each package has its own contract source owner.
  One package's contract artifact isn't used as the contract owner for a
  whole different package.
- Public constructors, factories, builder entrypoints, free functions,
  extensions, DTOs, values, enums, and public errors/results are owned by
  the same contract source as the interface.
- Runtime implementations reference the contract. Contract source doesn't
  reference the runtime implementation.
- Declarations under `runtime/internal` must not be importable from
  outside. Just renaming the directory to `internal` while keeping public
  visibility doesn't satisfy this rule.
- The minimal SPI an external provider must implement can be owned by a
  separate abstraction artifact. The entire application-facing contract
  isn't moved into the SPI artifact.
- Only the minimal contract that multiple packages must share type
  identity for — such as codec/error — can be owned by a
  package-neutral artifact.

The exact interface determines the namespace or package FQN. The FQN
isn't changed just to tidy up source layout. A layout change must pass
the public API snapshot, package consumer build, and owner gate together.
Each language's exact interface records the contract source owner and
public projection per package. If an exception owner is needed, the
target declaration and the dependency reason are recorded individually —
an exception isn't granted for a whole directory or assembly.

This boundary follows the same direction as the
[bindings public/internal boundary](../../../../../bindings/doc/spec/README.en.md#public-vs-internal-api-boundary).
Framework and bindings are different deployment packages, so each spec
owns its own source root, but the principle that the public contract
doesn't depend back on the runtime implementation is the same.

## 3. Required Items In The Common Contract

When defining a public feature, fix the following items together.

- The package the feature belongs to and its runtime owner
- The operation's input, result, and valid call timing
- Timeout, cancellation, and backpressure semantics
- Callback execution order and serialization scope
- Ownership of the message and result objects
- The distinction between configuration errors and runtime errors
- Selection criteria for automatic discovery vs. manual peer
- The results observable in contract tests and E2E

Listing function names alone doesn't complete a contract. It must also
explain the point at which the user can judge success and where a
failure is received.

## 4. Public Contract Procedure

Follow this order when adding or changing a public contract.

1. Record the common feature and the user-observed result in the common
   spec.
2. Record the exact public interface in each affected language spec.
3. Record the gap between the current checkout and the target contract in
   a temporary transition inventory. The formal spec doesn't reference
   this inventory or describe implementation progress.
4. If a Core or bindings contract is needed, align that package's formal
   spec first, then align the public header and implementation to that
   contract.
5. Verify that contract tests, the common E2E, and samples only use the
   public surface.
6. Cross-check the deployment package's actual exports against the
   document's signatures.
7. Approve the change only after an independent review confirms there's
   no gap between contract, implementation, tests, and package.

The common E2E and other languages' code are material for verifying
contract interpretation. They alone don't create a public interface. A
public interface must always be grounded in the formal spec.

## 5. Language-Specific Representation Principles

The language-specific interfaces keep the feature the same while
following these conventions.

- .NET uses `Task`, `ValueTask`, `CancellationToken`, and DI conventions.
- Java uses the Java type system and `CompletionStage` conventions.
- Kotlin uses `suspend`, `Flow`, and coroutine cancellation rules on
  surfaces that provide coroutines.
- Node.js uses `Promise`, TypeScript optional representation, and
  `AbortSignal` for long-running operations that need it.
- C++ uses explicit ownership, value types, and coroutine conventions.

One language's type names and overload composition aren't copied
verbatim into another language. If the feature, completion condition,
and error semantics are the same, it's considered a projection of the
same common contract.

## 6. Design Review Criteria

A new public interface must reduce the decisions the caller needs to
know.

- Node direct, channel select-one, and Logical Multicast provide
  selection and submit as one operation.
- Transport endpoint, peer selection, packet encoding, and reply
  correlation are owned by the runtime.
- Spot, Actor, and STREAM session addresses and generations are
  preserved by a typed handle or context.
- The same feature isn't repeated as interfaces that differ only in
  name.
- An invalid combination of states isn't represented by splitting it
  across multiple nullable values and booleans.
- Settings whose valid range differs per operation, like timeout or
  metadata, are only placed on that call object.

For a non-trivial design, compare at least two options and choose the one
whose public interface is smaller and exposes less transport knowledge.

### 6.1 Public Boundary Of The Language-Specific Exact Interface

The language-specific exact interface document only records the API the
application uses directly and the SPI an external provider package must
implement. Internal runtime wiring, storage rows/keys, state-transition
commands, change watches, publishers, dispatcher invocations, and native
diagnostics aren't the public contract. Even if such types are needed for
the implementation, they stay inside the package, and their
responsibility and behavior are explained in the common or
language-specific internals.

A feature that one provider can implement within the same consistency
boundary isn't split into fine-grained capability interfaces and exposed
to application registration. The minimal operations an external provider
needs to implement are gathered into one deep SPI, and optional features
are absorbed into the default implementation or a capability query. An
exact interface document with no remaining public declaration for an
outside implementer or caller is deleted. This standard applies equally
to .NET, Java, Kotlin, Node.js, and C++.

## 7. Verification

Each language's contract tests confirm at least the following.

- Public exports importable from an external package
- Application-facing public declarations outside the contract source
  each package defines
- Reverse dependencies from contract source toward runtime, internal, or
  native bridge source
- Whether declarations in runtime/internal source are actually blocked
  by package/module/assembly visibility
- Public type and method signatures
- Generics, nullable, optional, defaults, and overloads
- Async results, timeout, and cancellation
- Public error kinds and lifecycle callbacks
- Messaging contracts per MeshName, ChannelName, RID, and
  [owner](01-glossary.en.md#owner)
- Explicit registration of Redis Location Store and manual peer
  configuration

Verification doesn't only look at the source tree. It confirms that an
external consumer referencing the actual deployment package gets the
same public surface and behavior.

## 8. The 11.0 Spec-First Authority Rule

11.0's Core service migration and service runtime restructuring first fix
the formal spec and internals as the authority for the target state.
Planning documents, drafts, the current implementation, and other
languages' implementations aren't the source of the contract. The
implementation fills the gap against the approved authority, and if a
constraint that requires changing the contract is found during
implementation, the affected spec and internals are re-reviewed and
changed together, instead of bypassing the source.

The formal spec only describes the user-observed target contract.
Per-language implementation gaps, removal progress, and test status are
owned by each language's audit/execution ledger. Internals describe the
target runtime's responsibility boundary, data flow, and invariants, and
don't include migration history or a progress table.

Core owns raw transport, and the C++/.NET/JVM/Node.js Frameworks each own
their own language's service runtime independently. A common native
Framework runtime, a private C SPI, and a service C ABI aren't created.
The five-language public contract projects the common Framework spec, and
the four runtimes align observable results using a common protocol
schema and fixtures. Java and Kotlin each provide their own public
contract but share the Java binding and JVM runtime implementation.

Moving the Core service runtime into each language's Framework internals
isn't grounds for changing the existing Framework public contract.
Channel/[Spot](01-glossary.en.md#spot)/Actor/STREAM's existing public
symbols, signatures, and completion semantics stay the same. When a
public change is needed in 11.0, a separate feature basis — such as new
application intent, or a direct conflict with an existing contract, as
with maintenance — must be presented in the common spec. Public renames,
wrappers, and backend-selection options that arise from a purely internal
migration aren't allowed.

The common spec's concept names and data types aren't an instruction to
change an existing language's public types. If a public type, generic,
callback argument, or default interface implementation already expresses
the same meaning, that surface is kept. For example, don't change .NET's
typed `IZLinkUserSpotActorLifecycle<TActor>` into a non-generic callback
just because a common Actor membership record was newly defined. Only
when the common semantics can't be expressed with an existing surface is
a minimal member added, and only after a separate feature basis and POSD
review.

The language-specific exact interfaces are owned by the per-category
documents under `languages/<lang>/interfaces/`. Splitting files and
changing the table of contents isn't an API change. When splitting an
existing single catalog, move each public member exactly once without
loss, and don't duplicate the same declaration across multiple
categories.
