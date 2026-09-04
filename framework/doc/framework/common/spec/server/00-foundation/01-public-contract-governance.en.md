---
title: "Public Contract Governance"
---

# Public Contract Governance

[Foundation Topic Index](README.en.md) · [Spec Index](../README.en.md) · [Next: 02. Framework Messaging Glossary](02-glossary.en.md)

> Defines who owns the Framework public contract, and how a new contract is fixed and verified.

## 1. What the Public Contract Is

The public contract consists of more than the types and operations an application calls. It also
includes timeout, cancellation, error, callback execution order, ownership, and completion
conditions. This document defines the ownership and verification rules for this contract.

## 2. Contract Ownership

The public contract is split into common semantics and language-specific representation.

| Location | Content owned |
|---|---|
| This directory and the common spec per package | Language-independent features, states, completion conditions, error semantics |
| A package's `languages/<lang>/` | Actual public types, method signatures, generic/nullable rules, language-specific async representation |
| The Core formal spec | context, message, raw socket, transport, poller, and generic monitoring contracts |
| This directory's implementation-spec narrative | Service-runtime wiring, state machines, protocol handling, and thread/executor structure that all languages share to satisfy the public contract. They don't own a new public contract. |

The common spec doesn't take any one language's syntax as the standard. Each language expresses
the same feature and observable result in its own idiom. The precise signature of .NET
[RouteMesh](02-glossary.en.md#routemesh) — the scope in which multiple MeshNodes participate to
exchange node and Channel messages — and [MeshNode](02-glossary.en.md#meshnode) — a runtime node
that participates in a RouteMesh to send or receive messages — is owned by
[.NET RouteMesh/MeshNode Interface](../languages/dotnet/interfaces/03-configuration-topology.en.md).

- **User-observable behavior is defined exactly once, in this directory's common spec and the
  language-specific interface.** The same document may go on to describe the implementation
  structure that produces that behavior, but it doesn't redefine the public behavior or extend
  it into a stronger guarantee — because the public contract would no longer have a single owner.
- **If an implementation-structure statement conflicts with public behavior or the public API,
  that conflict is a defect.**
  - The conflict isn't left in place by declaring that either side takes precedence.
  - The implementation-structure statement is corrected to match the contract,
    and if the implementation can't follow that structure and the public behavior itself must
    change, [§5 Public Contract Procedure](#5-public-contract-procedure) is followed again.
  - A gap
    report distinguishes public-contract gaps, implementation-structure gaps, and missing
    verification evidence.

## 3. Production Source Owner

Each deployment package has exactly one source-file location for the interfaces, calls, contexts,
options, results, and errors that the application references at compile time. This document calls
that location the "contract source." Source directory names follow each language's convention,
but the following rules apply to every language.

- **The application contract source is owned by that deployment package.**
- **Server, HTTP Client, and Stream Connector are each independent deployment packages, so each
  package has its own contract source owner.** One package's contract artifact isn't used as the
  contract owner for a whole different package — otherwise, consumers of that package would
  depend on a package they don't use.
- **Public constructors, factories, builder entrypoints, free functions, extensions, DTOs,
  values, enums, and public errors/results are also owned by the same contract source as the
  interface.** Scattering this list across a source different from the interface means the owner
  isn't singular.
- **Runtime implementations reference the contract. Contract source doesn't reference the
  runtime implementation.** If this direction is reversed, the contract ends up depending on
  implementation detail, and every implementation change can destabilize the contract.
- **Declarations under `runtime/internal` must not be importable from outside.** Just renaming
  the directory to `internal` while keeping public visibility doesn't satisfy this rule.
- **The minimal SPI an external provider must implement can be owned by a separate abstraction
  artifact.** For example, a [Location Store](02-glossary.en.md#location-store) provider — the
  store that lets multiple nodes check each Spot's current owner and lifecycle state — is such a
  case: an implementation built outside the Framework only needs to reference that one artifact.
  The application-facing contract isn't moved into the SPI artifact — otherwise, the application
  would depend on the provider artifact.
- **Only codec and error, which multiple packages must exchange as the same types, can be owned by
  a package-neutral artifact.** Declaring types with the same names separately in each package
  would make them different types, preventing values from being exchanged. Moving any other
  contract into a package-neutral artifact blurs which package owns each contract.

The language-specific interface determines the full namespace or package name — the name joined
from its top-level component, such as `systems.zlink.framework.actors`. This name isn't changed
just to tidy up the source layout. Because it is embedded directly in the code of package
consumers, changing it would break those consumers even without a contract change.

A layout change must pass all three checks together: comparison against the recorded public API
list, the build of a consumer of that package, and owner review.

Each language-specific interface records the contract source location and public projection for
each package. If a declaration must be placed elsewhere, that declaration and the dependency
reason must be recorded individually. An exception isn't granted for a whole directory or
assembly, because that would make it impossible to track what is exceptional.

This boundary follows the same direction as the
[bindings public/internal boundary](../../../../../../../bindings/doc/spec/README.en.md#public-vs-internal-api-boundary).
Framework and bindings are different deployment packages, so each spec owns its own source root,
but the principle that the public contract doesn't depend back on the runtime implementation is
the same.

## 4. Items to Fix in a New Contract

When defining a public feature, fix the following items together.

- The package the feature belongs to and its runtime owner
- The operation's input, result, and valid call timing
- Timeout, cancellation, and backpressure semantics
- Callback execution order and serialization scope
- Ownership of the message and result objects
- The distinction between configuration errors and runtime errors
- Selection criteria for automatic discovery vs. manual peer
- The results observable in contract tests and E2E

Listing function names alone doesn't complete a contract. It must also explain the point at
which the user can judge success and where a failure is received.

## 5. Public Contract Procedure

Follow this order when adding or changing a public contract.

1. Record the common feature and the user-observed result in the common spec.
2. Record the precise public interface in each affected language spec.
3. Record the gap between the current checkout and the target contract in a temporary
   transition inventory. The formal spec doesn't reference this inventory or describe
   implementation progress.
4. If a Core or bindings contract is needed, align that package's formal spec first, then align
   the public header and implementation to that contract.
5. Verify that contract tests, the common E2E, and samples only use the public surface.
6. Cross-check the deployment package's actual exports against the document's signatures.
7. Approve the change only after an independent review confirms there's no gap between contract,
   implementation, tests, and package.

**The common E2E and other languages' code are material for verifying contract interpretation
only, not grounds for a public interface.** The public interface must always be grounded in the
formal spec.

## 6. Language-Specific Representation Principles

The language-specific interfaces keep the feature the same while following these conventions.

- .NET uses `Task`, `ValueTask`, `CancellationToken`, and DI conventions.
- Java uses the Java type system and `CompletionStage` conventions.
- Kotlin uses `suspend`, `Flow`, and coroutine cancellation rules on surfaces that provide
  coroutines.
- Node.js uses `Promise`, TypeScript optional representation, and `AbortSignal` for long-running
  operations that need it.
- C++ uses explicit ownership, value types, and coroutine conventions.

**One language's type names and overload composition aren't copied verbatim into another
language.** If the feature, completion condition, and error semantics are the same, it's
considered a projection of the same common contract.

## 7. Design Review Criteria

A new public interface must reduce the decisions the caller needs to know.

- **Node direct, channel select-one, and Logical Multicast provide selection and submit as one
  operation.** This is so the caller isn't made to look up a candidate and then call a separate
  send again.
- **Transport endpoint, peer selection, packet encoding, and reply correlation are owned by the
  runtime.** If an application manages these values directly, wiring that differs by language
  leaks into the public surface.
- **The addresses and generations of [Spot](02-glossary.en.md#spot) — a logical instance that
  represents a message recipient — Actor, and STREAM sessions are preserved by a typed handle or
  context.** This is so the caller isn't made to reassemble the value on every call.
- **The same feature isn't repeated as an interface that differs only in name.**
- **An invalid combination of states isn't represented by splitting it across multiple nullable
  values and booleans.**
- **Settings whose valid range differs per operation, like timeout or metadata, are placed only
  on that call object.**

For a non-trivial design, compare at least two options and choose the one whose public interface
is smaller and exposes less transport knowledge.

The same boundary applies to the language-specific interface documents as well.

- **The language-specific interface document only records the API the application uses
  directly and the SPI an external provider package must implement.** Internal runtime wiring,
  storage rows/keys, state-transition commands, change watches, publishers, dispatcher
  invocations, and native diagnostics aren't the public contract. Even if such types are needed
  for the implementation, they stay inside the package, and their responsibility and behavior are
  explained in this directory's implementation-spec narrative or the language-specific
  implementation documents.
- **A feature that one provider can implement within the same consistency boundary isn't split
  into fine-grained capability interfaces and exposed to application registration.** The minimal
  operations an external provider needs to implement are gathered into one deep SPI, and optional
  features are absorbed into the default implementation or a capability query.
- **A language-specific interface document with no remaining public declaration for an outside implementer
  or caller to implement or call is deleted.** This standard applies equally to .NET, Java,
  Kotlin, Node.js, and C++.

## 8. Verification Requirements

The following is confirmed using only the contract tests and actual deployment package for each
language (the public export importable from an external package, the contract source defined for
each package, the signature, the async result, and the messaging contract per
[owner](02-glossary.en.md#owner)).

**Export and dependency direction**

- The public export importable from an external package comes only from within the contract
  source defined per package — there's no application-facing public declaration outside the
  contract source.
- There's no reverse dependency from the contract source toward runtime, internal, or native
  bridge source.
- Declarations in `runtime/internal` source are actually blocked by package/module/assembly
  visibility so they can't be imported from outside.

**Signature and representation**

- The public type and method signatures match the language-specific per-language interface.
- Generics, nullable, optional, defaults, and overloads are exposed exactly as the
  language-specific interface defines them.

**Completion and errors**

- The async result, timeout, and cancellation are observed according to the language-specific
  convention ([§6](#6-language-specific-representation-principles)).
- The public error kind and lifecycle callback are invoked as the document defines.

**Messaging and store registration**

- [MeshName](02-glossary.en.md#meshname) — the name identifying one RouteMesh physical connection
  group — [ChannelName](02-glossary.en.md#channelname) — the name identifying the Channel scope a
  message is sent to — RID, and the messaging contract per [owner](02-glossary.en.md#owner)
  match the document.
- The explicit registration of the Redis location store and manual peer configuration match the
  document.

Verification doesn't look only at the source tree. It confirms that an external consumer
referencing the actual deployment package gets the same public surface and behavior.

---

[Foundation Topic Index](README.en.md) · [Spec Index](../README.en.md) · [Next: 02. Framework Messaging Glossary](02-glossary.en.md)
