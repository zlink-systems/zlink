---
title: "Bindings API Policy"
---

<!-- bindings-nav:start -->
[Spec index](README.md)
<!-- bindings-nav:end -->

# Bindings API Policy

> **What this chapter defines** — the public API policy that applies across
> all of `bindings/`. The per-language documents (`c/`, `cpp/`, `java/`,
> `dotnet/`, `node/`, `python/`, `go/`, `rust/`) align to this policy.

> The implementation baseline for request-reply, SPOT routed, and Actor
> dispatch follows the current public contract in `core/include/zlink.h`.
> Actor dispatch is an independent public service-layer capability, like
> SPOT, and the public surface described in each per-language document
> aligns to this shared contract as well.
> See `c/`, `cpp/`, `java/`, `dotnet/`, `node/`, `python/`, `go/`, `rust/`
> for the per-language interface signatures and usage examples.

| Section | Covers |
|---|---|
| [Purpose](#purpose) | This document's scope and the meaning of the Required/Target notation |
| [Binding Contract Category Policy](#binding-contract-category-policy) | Contract category classification |
| [Binding Runtime Category Policy](#binding-runtime-category-policy) | Runtime category classification |
| [Actor/Spot Route Surface](#actorspot-route-surface) | Route lookup result types, and Actor-targeted send/request |
| [High-Performance Binding Policy](#high-performance-binding-policy) | Hot-path constraints |
| [Substrate vs Public Binding Surface](#substrate-vs-public-binding-surface) | The boundary between the part substrate and the aggregate public surface |
| [`*_part` Substrate Usage Requirement (Required)](#_part-substrate-usage-requirement-required) | Why an aggregate implementation must use the `*_part` API |
| [Spot Get-Or-Create Mapping](#spot-get-or-create-mapping) | The `zlink_spot_node_spot_get_or_new` mapping rule |
| [Public vs Internal API Boundary](#public-vs-internal-api-boundary) | The contract/runtime separation principle and its test |
| [Core Alignment Rules](#core-alignment-rules) | Alignment rules against the core contract |
| [Actor Dispatch Binding Contract](#actor-dispatch-binding-contract) | The public Actor dispatch surface |
| [Document Interpretation Rules](#document-interpretation-rules) | How to read the Required/Target notation |
| [Core Principles](#core-principles) | The core principles that run through this whole policy |
| [Monitor Ready Contract](#monitor-ready-contract) | The meaning of monitor readiness |
| [POSD Structure Policy](#posd-structure-policy) | The deep-module, low-change-amplification structural standard |
| [Public Surface Rules](#public-surface-rules) | Operation naming, builder, and terminator rules |
| [Domain Object Policy](#domain-object-policy) | The value-type-vs-interface test |
| [Socket Type Capability Policy](#socket-type-capability-policy) | The capabilities each socket family exposes |
| [Per-Language Spec File Compliance Rule](#per-language-spec-file-compliance-rule) | The relationship between the per-language documents and this document |
| [Service Layer Policy](#service-layer-policy) | The public contract for the SPOT/Actor service layer |
| [Core API Additions](#core-api-additions) | Binding coverage for recently added core capabilities |
| [Option Policy](#option-policy) | Rules for exposing socket/context options |
| [Performance Policy](#performance-policy) | Shared performance standards across all bindings |
| [Boundary Cost Policy](#boundary-cost-policy) | The FFI/marshalling boundary-cost standard |
| [Peer Weight Policy](#peer-weight-policy) | The peer-weight contract |
| [Monitor Policy](#monitor-policy) | The monitor event/snapshot contract |
| [Error Policy](#error-policy) | Error representation and domain mapping |
| [Length And Range Boundary Policy](#length-and-range-boundary-policy) | Value validation and boundary limits |
| [Ownership Policy](#ownership-policy) | Message/handle ownership rules |
| [Naming Policy](#naming-policy) | Shared naming rules across languages |
| [Compatibility Policy](#compatibility-policy) | The ban on compatibility shims and deprecated wrappers |
| [Cross-Language Alignment](#cross-language-alignment) | How to verify consistency across languages |
| [Test Policy](#test-policy) | Test scope and standards |
| [Test Matrix](#test-matrix) | A language-by-capability coverage table |
| [Sample Policy](#sample-policy) | Sample code standards |
| [Perf Policy](#perf-policy) | Perf-runner standards |
| [Script Location Policy](#script-location-policy) | Where test/sample/perf scripts live |
| [Review Checklist](#review-checklist) | Checks to make during PR review |
| [POSD-Based Implementation Completeness Policy](#posd-based-implementation-completeness-policy) | The test for declaring an implementation complete |
| [Implementation Review Checklist](#implementation-review-checklist) | Checks before declaring an implementation done |
| [Binding Requirements](#binding-requirements) | Requirements every binding must satisfy |
| [API Reference](#api-reference) | Standards for generating API reference documentation |
| [Disconnecting A Peer By Routing ID](#disconnecting-a-peer-by-routing-id) | The routing-id-based peer disconnect contract |
| [Related Documents](#related-documents) | Links to related documents |
| [Core API Surface 6.0.0 Alignment](#core-api-surface-600-alignment) | Alignment status for the 6.0.0 core API surface |
| [Spot Route Bridge API](#spot-route-bridge-api) | The route bridge API contract |

## Purpose

This document defines the public API policy for all of `bindings/`.

Its purpose is to prevent each language binding from growing its own
surface and its own exception rules, and instead to enforce a shared
contract that can always be explained in terms of
`core/include/zlink.h`.

This document does not claim that every binding is already in this state
today. It sets the `.NET` binding's contract/runtime separation and file
granularity as the standard target, and gives the remaining wrapper
bindings a baseline to align to in stages. Items marked `Required` apply
immediately in the current review; structures and surfaces marked
`Target` apply as the goal of the work that aligns that binding. The C
binding is the native ABI baseline, so it follows a separate set of
exceptions.

The documents under `c/`, `cpp/`, `java/`, `dotnet/`, `node/`, `python/`,
`go/`, `rust/` define the public API contract each binding implementation
must actually provide externally. What these documents govern is the
public types, methods, signatures, return values, and error semantics —
the public interface a binding implementation exposes must not diverge
from this contract. Every wrapper binding except C also separates the
public contract from the runtime implementation. This separation is fixed
by responsibility, but the physical directory and package/module path
follow each language's conventions. The actual path each per-language
README specifies is the implementation baseline for that binding.

This document is not a simple style guide. It is a design-standard
document for:
- public API design standards
- review standards
- refactoring standards
- sample and test standards

Its intent is to:
- eliminate APIs that look similarly named across languages but carry
  different meaning
- eliminate shallow surfaces that expose the same capability through
  multiple redundant paths
- reduce raw option bags, unnecessary convenience wrappers, implicit
  ownership, and hidden error paths
- let a binding user avoid needing to know internal sequencing, native
  detail, or hidden transport switches
- drive deep modules and low change amplification, per POSD principles
- tie correctness together with the cost model, sample quality, and
  testability under one shared standard

The baseline is always `core/include/zlink.h`. Each binding follows the
core contract, and may choose its representation to fit language
convention — but the semantic contract must not change.

This document defines "what each language must guarantee," not "how each
language may look."

## Binding Contract Category Policy

Every binding must split its public contract into the same semantic
categories. The actual folder, package, namespace, or module name can be
adjusted to fit language convention, but which category a given public
type belongs to must be judged by the same standard across bindings.

The point of this policy is not to make file placement look tidy. It is
to let a user find a concept they learned in one language, at the same
location and with the same meaning, in another language. So the contract
subcategories follow the conceptual boundaries of the public API, not the
implementation file structure.

| Category | Purpose | Includes |
|------|------|-----------|
| `core` | The library-wide foundational contract | Public types not tied to a specific socket or service, such as `Context`, `ContextOptions`, `RoutingId`, and version/capability lookup |
| `messaging` | The message data and receive-result contract | Payload types independent of socket kind, such as `Message`, `Received`, topic message, subscription event, and multipart payload helpers |
| `sockets` | The socket-kind and socket-operation contract | `PairSocket`, `DealerSocket`, `RouterSocket`, `PubSocket`, `SubSocket`, `StreamSocket`, socket interfaces, send/recv/publish/request/reply builders, socket options |
| `eventing` | The waiting, event-source, and observation contract | `Poller`, `PollEvent`, timer, monitor socket, monitor event, monitor snapshot |
| `service` | The core service-layer contract | Public types that belong to a service domain, such as Spot and Actor dispatch |
| `errors` | The public error and failure-representation contract | Base exception, bind/connect/send/recv/submit/config/request exceptions, public error-code/result mapping |

### Contract Category Rules

- The `contract`/`runtime` split is the split between the public API and
  implementation detail. A contract subcategory must not mirror the
  runtime's internal structure as-is.
- Keep `core` small. A type that can only be explained by knowing a
  specific domain belongs in that domain's category, not in `core`.
- `service` can have subdomains such as `spot` and `actor`. Create a
  subdomain only when a user must learn it as an independent concept.
- `eventing` does not mean monitoring alone. It also holds public
  contracts for waiting on or observing events, such as poller, timer,
  and monitor.
- `errors` holds error surfaces shared across multiple domains. A type
  whose meaning is strongly domain-specific, such as the result of a
  particular socket operation, belongs in that domain instead.
- Do not make a representation-format category such as `enums` a
  canonical category. Enums, flags, and results belong in the category of
  the public concept that interprets their value.
- Put operation, result, and callback helper types in the domain that
  defines their meaning. For example, send/request/reply results and
  callbacks belong in the messaging contract, while Actor
  join/session/management results and callbacks belong in the service
  contract. A snapshot entry stays with the service model that returns
  that snapshot.

### Handler Registration Naming Policy

A callback/handler registration function's name must reveal what it
actually does. A name that reads like a function invoked when an event
occurs, used for a registration function, can make a user unsure whether
it's a hook they must implement or an API that stores a handler.

- A public API that stores a single handler for one subject, or replaces
  the existing handler, uses a `set...Handler`-family name. Spell it per
  language convention: `Set...Handler`, `set...Handler`, or
  `set_..._handler`.
- A public binding's `set...Handler` keeps only one active handler per
  subject. Calling the same setter again replaces the current handler. A
  raw native attach conflict or a recv-mode conflict may still be
  reported as a separate error, but the public setter name does not imply
  cumulative registration.
- Only a public API that accumulates multiple handlers uses an
  `add...Handler` or `register...Handler`-family name.
- An `on...`-family name is reserved for a protected/internal hook or a
  framework-level handler method invoked when an event occurs. It is not
  the canonical name for a handler-registration function.
- An API that changes protocol state, such as a topic subscription, may
  use `subscribe`/`unsubscribe`. A function that simply stores a callback
  does not use `subscribe...Handler`.
- Do not create a surface that unregisters by setting a callback to
  `null`/`None`. When unregistration is needed, handle it through the
  close/lifecycle rules instead.

The representative canonical semantic names are:

| Meaning | Canonical name |
|------|----------------|
| Registering a send-ready handler | `setSendReadyHandler` |
| Registering a raw STREAM packet handler | `setPacketHandler` |
| Registering a SPOT dispatch event handler | `setDispatchHandler` |
| SPOT routed receive | `recvRouted` |
| SPOT Actor lifecycle receive | `recvActorLifecycle` |

The representative enum/result/flags placement rules are:

| Example type | Category | Reason |
|---------|------|------|
| `SendFlags`, `RecvFlags`, `SubmitResult`, `RecvResult` | `sockets` | Describes the input to or result of a socket operation. |
| `PollEventFlag`, `PollSourceKind`, `MonitorEventType` | `eventing` | Describes an event-wait or observation result. |
| `SpotDispatchEvent`, `SpotPeerKind` | `service.spot` | Its meaning is defined only inside the Spot service domain. |
| `ConfigResult`, `ErrorCode` | `errors` | Describes a failure meaning shared across multiple domains. |

Every wrapper binding shares the same architecture map. This map is not a
baseline for copying one language's folder names literally — it lets a
concept learned in one language be found at the same responsibility
location in another language's code. The actual file names, directory
names, and package/module/import paths follow per-language convention and
public API stability.

The shared architecture map is:

```text
contracts/
  core/
  messaging/
  sockets/
  eventing/
  service/
  errors/

runtime/
  native/
  sockets/
  messaging/
  eventing/
  service/
  errors/
  buffers/
  options/
  handles/
```

In this map, `contracts` is the public contract surface a user reads
first. `contracts` holds the types a user directly depends on: public
interfaces, public value objects, public result/flag/error types, and
public builders/facades. Implementation detail stays hidden in `runtime`.

Do not overuse public interfaces, however. Value objects and plain data
types such as `Message`, `RoutingId`, `Received`, `TopicMessage`, and
enum/result/flags are not split into separate interfaces. Interfaces
belong only where a user needs to receive something polymorphically —
for example, a common socket role, a poll target, a monitor target, a
codec, a handler/callback, or a SPOT client role.

`runtime` is the implementation area that executes the public contract.
It hides implementation decisions such as the socket send/recv flow,
message materialization, the poller/timer/monitor loop, the service
runtime, native interop, and buffer/handle/error mapping. A runtime type
is not recommended as public API, and a user should not depend on it
directly without going through the contract surface.

### Interface / Implementation Separation Policy

Every wrapper binding except C follows the `.NET` binding's direction of
separating the public interface/contract from the runtime implementation.
Separating "like `.NET`" here does not mean every language copies names
such as `IContext` or `Contracts`/`Runtime` literally. It means the
contract a user sees and the implementation detail — native calls, handle
owners, callback bridges, request pumps — live in separate areas of
responsibility.

The separation rules are:

- Types, interfaces, traits, protocols, abstract roles, factories,
  builder start points, DTOs, value objects, enums, and error/result
  types that a user depends on go in the public contract source.
- A type that directly owns a native handle, calls the core helper
  substrate, manages a callback trampoline and request progress, or
  performs marshalling goes in the runtime or native bridge source.
- For a resource type where it's more natural for a user to depend on a
  role than on an implementation — `Context`, socket, poller, timer,
  SpotNode, Spot, Actor — separate the contract role from the default
  implementation in whatever way the language supports.
- Value-centric types such as `Message`, `RoutingId`, `Received`,
  `TopicMessage`, snapshot DTOs, and enum/flags/result are not wrapped in
  a meaningless interface/trait/protocol. Keep a value type as a concrete
  public type, and hide implementation detail inside the type itself when
  it needs internal native-backed storage.
- Even in a language where a runtime concrete class must be publicly
  exposed, the behavior contract a user needs to understand must be
  explained in the public contract source first.
- Write samples, perf, and framework adapters against the public contract
  projection, not against a runtime concrete type or native bridge.

This separation is not just a naming split. A file on the `contracts`
side must be readable without knowing the native handle, native function
names, request pump, callback trampoline, or buffer-marshalling sequence.
Conversely, a file on the `runtime` side implements the public contract,
but must not itself become a public surface a user has to import.

The same standard applies to file structure.

- Use a category aggregate file only as a small re-export barrel or for
  factory wiring. If a single category file such as `sockets`, `service`,
  or `eventing` holds the actual behavior of several public resources,
  the contract/runtime split has not happened.
- Put a native-backed resource's implementation in its own per-resource
  file. For example, each socket family, poller, timer, SpotNode, Spot,
  and Actor should have its own implementation file. The file name
  follows language convention but must reveal the resource or operation
  name.
- A shared helper file is not a substitute location for a public
  resource's implementation. A helper file should hold only lower-level
  functionality shared across multiple implementations, such as a native
  call wrapper, handle validation, a marshalling helper, or error
  mapping. Do not collect a resource's actual behavior — for `Context`,
  `RouterSocket`, `SpotNode`, `Poller` — into a helper file.
- A contract file and a runtime file do not need a strict 1:1 mapping,
  but for any given public resource, its contract owner and runtime owner
  must each be clear.

### File Granularity Policy

Every wrapper binding also matches a similar granularity when splitting
files. The goal is not to replicate file count or file names 1:1, but to
let a reader of any language find the same conceptual grouping in a
similarly sized file.

The baseline is the `.NET` binding's `Contracts` organization level. One
file holds either one independent public concept, or a small, tightly
coupled group of contracts that share the same reason to change.

The file-splitting rules are:

- A resource contract a user looks up directly — `Context`, a socket
  family, `SpotNode`, `Spot`, `Actor`, poller, timer — can have its own
  file even if it's thin.
- A type where ownership, storage, value validation, or cost model
  matters — `Message`, `Received`, `TopicMessage`, `RoutingId` — gets its
  own file.
- A type with a weak independent reason to change — a marker interface,
  delegate, small enum, one-line record — merges into the nearest
  contract file. For example, a socket marker role merges with the
  socket base contract, and a stream packet handler delegate merges with
  the stream socket contract.
- Staged operation builder contracts such as send/request/reply share the
  same domain-level reason to change, so they can be grouped into one
  operation contract file.
- A group with a clear service subdomain — Actor join, actor management,
  SpotNode snapshot models — gets its own domain file. Split it further
  only once the model file grows large enough that distinct reasons to
  change appear, such as peer/status/socket/actor snapshots.
- The same principle applies to runtime implementation files. One
  implementation file holding several native-backed resources' lifecycle,
  send/recv/request flow, callback registration, and snapshot mapping all
  at once is too broad. Split such a file into per-resource
  implementation files and a shared helper file.
- Keep a category barrel small. As a rough guideline, once a barrel holds
  hundreds of lines of resource implementation beyond re-exports and
  simple factory wiring, treat that as a split failure. The real
  criterion is the reason to change, not the line count — if editing
  different public resources always means opening the same file, split
  it.
- Do not create a file based only on a representation format or a
  catch-all name such as `Enums`, `Types`, `Models`, `Common`, or `Utils`.
  A file name must reveal the domain concept a user is looking for, or
  the reason it changes.
- Each language follows its own casing, suffix, and package convention.
  For example, C# might spell it `OperationContracts.cs`, Rust
  `operation_contracts.rs`, and TypeScript `operation-contracts.ts` — but
  the same grouping of responsibility must be preserved.

When applying this standard, the alignment approach declared by the
per-language README takes priority. If a per-language README declares a
breaking alignment, aligning to the canonical surface takes priority over
preserving the existing public surface. Prefer to cleanly rearrange the
namespace, package export, crate re-export, package `exports`, or
generated declaration surface when moving files. Do not create a new
public wrapper or a shallow compatibility shim just to match the file
structure.

The per-language application follows these principles:

- `.NET` treats `Contracts/<Category>` and `Runtime/<Category>` as its
  standard structure. Public interfaces and public value objects go in
  `Contracts`; implementation classes and native interop helpers go in
  `Runtime`.
- Because a Java package is close to public API, Java puts public
  interfaces/value objects under `systems.zlink.contracts.<category>`,
  and implementation classes and native bridges under
  `systems.zlink.runtime.<category>` or
  `systems.zlink.runtime.nativeapi`. It does not create a `FooContract`
  interface just to list methods.
- C is the native ABI baseline, so it does not create separate
  contract/runtime folders. It expresses the same categories through
  header files, header sections, and documentation sections.
- C++, Go, Rust, Python, and Node follow each language's module/package/
  export convention, but must still distinguish the public-facing surface
  from the runtime implementation. When a language does not naturally
  support interfaces, the same distinction must be made explicit in the
  documentation and the export surface.

If an existing binding has a `monitoring` or `Monitoring` category, treat
`eventing` as the canonical category. The name `monitoring` was
sufficient while only the monitor API existed, but `eventing` is the
broader and more accurate concept for a public contract that also covers
poller and timer. When cleaning up structure, describe new documents and
new files as `eventing` responsibility. An already-public `monitoring`
import/export path may be kept as a temporary alias only when the
matching per-language README explicitly commits to preserving
compatibility. A binding that has declared a breaking alignment cleans up
to `eventing` and does not keep a `monitoring` alias.

A representation-format folder such as `enums` is not a top-level
category in the shared architecture map. Enums, flags, results, and
literal unions belong in the domain category that interprets their
value. For example, `RecvFlags` belongs to the `sockets` contract,
`PollEventFlags` to `eventing`, and `SpotPeerKind` to the `service`
contract.

## Binding Runtime Category Policy

A wrapper binding separates the public contract from the runtime
implementation. Runtime is the implementation layer that performs the
actual behavior behind the contract surface. The public contract shows a
user what they can call, and runtime handles that call against the
native substrate and the language's own execution model.

A runtime subcategory does not need to be strictly 1:1 with a contract
subcategory. But the implementation responsibility and reason to change
must be clear, and it must not grow into a thin pass-through class that
merely repeats the public contract.

The recommended runtime categories are:

| Category | Responsibility |
|------|------|
| `native`, or a per-language equivalent name | P/Invoke, JNI, FFI, native function declarations, ABI type conversion, native symbol loading |
| `handles` | Native handle ownership, dispose/close, lifetime, reference tracking |
| `messaging` | Native message part assembly, multipart handling, message conversion, request progress |
| `sockets` | Socket operation execution, send/recv/publish/request/reply flow |
| `eventing` | Poller, timer, monitor, event dispatch loop |
| `service` | Spot, Actor service runtime |
| `options` | Public option validation, native option mapping |
| `errors` | Converting native errno/result into public exception/result |
| `buffers` | Byte buffer, direct buffer, pooled buffer, pinned memory, copy/borrow policy |

The name may differ because of a language reserved word. For example,
because `native` is a keyword in Java, it can use `runtime/nativeapi`
instead. Names can differ, but the responsibility must still be
explainable.

### Runtime Category Rules

- Runtime does not promise public API stability. The public contract is
  defined by the contract documentation and the per-language public
  surface.
- A runtime implementation class hides behind a public contract interface
  or a public facade. If a user has to construct or call a runtime class
  directly, the contract design is leaking.
- If a contract interface only repeats the runtime implementation's
  method list 1:1, that's a shallow-module warning sign. Create an
  interface only where there's a real role abstraction, and never for a
  value object.
- Split runtime categories by reason to change. For example, adding a
  native symbol should change `native`; a send/recv flow change should
  change `sockets`; a message-ownership change should change `messaging`
  or `buffers`.
- Do not use a catch-all name such as `core`, `common`, `utils`,
  `internal`, or `misc` as a canonical runtime category. These names make
  it easy to mix unrelated reasons to change into one place.

If an existing runtime has a `monitoring` or `Monitoring` category, the
canonical category is `eventing`, the same as for contract. Even a file
that only holds a monitor implementation belongs under `eventing` if it
shares a reason to change with the poller, timer, or event dispatch loop.

From a POSD perspective, this standard aims to get both public-surface
readability and implementation information hiding at once. The contract
gives users a small, clear surface to learn, while runtime absorbs
implementation decisions such as how native calls are made, handle
ownership, buffer pooling, and error mapping. Do not create an
abstraction, however, when it doesn't actually reduce a real role and
only repeats a method list — that adds complexity instead of reducing it.

## Actor/Spot Route Surface

Every binding must expose the core's Actor route and Spot route results
without loss. Per-language type names can differ, but the following
meaning must be preserved.

- An Actor route exposes the Actor ref's node rid, current Spot rid, and
  current Spot kind.
- A Spot route exposes the looked-up Spot rid, owner node rid, and Spot
  kind.
- Spot kind distinguishes Entry Spot, user Spot, and an invalid value.
- A binding does not create a new direct `router -> actor` or
  `actor -> router` API. A user combines a route lookup result with the
  existing Spot routed API.

## High-Performance Binding Policy

zlink is a high-performance messaging library. A binding may add
per-language convenience, but it must not hide or worsen the hot path's
cost model. The public API and internal implementation must follow the
principles below.

- Do not use reflection-based dynamic dispatch on the message send/recv,
  publish/subscribe, request/reply, dispatch callback, poller, or timer
  path. Even when a language runtime requires reflection, restrict it to
  initialization or binding-registration time, and never use it in the
  message-processing loop.
- Reflection is not a workaround for a missing API. A high-performance
  binding must use a typed facade, a direct native downcall, or a direct
  internal bridge, and must not add a reflective lookup to the hot path
  just to satisfy the public contract.
- Do not create unnecessary allocation. A repeated call must not
  construct a new temporary array, wrapper, closure, or boxed object of
  the same size every time.
- Do not create unnecessary copies. Move a message part received from
  core into the language's own `Message`-owned object as directly as
  possible, and do not copy the byte buffer again without a decode step
  or an explicit user request.
- Do not put a global lock, coarse lock, avoidable mutex contention, or a
  shared-executor serialization point on the hot path. Limit necessary
  synchronization to the minimum scope that protects per-subject state.
- Do not perform a hidden blocking wait, sleep, busy wait, or thread join
  on the callback, dispatch, poller, timer, or request-completion
  progress path. Only a call explicitly documented as a blocking API may
  wait.
- A binding uses core's `*_part` substrate to build language objects part
  by part. Double materialization — building a native aggregate array and
  then converting it again into a language-specific collection — is
  forbidden.
- Perf, sample, and test code used for performance verification must also
  use only the public binding entrypoint, and must not break the cost
  model above.

This section is not an implementation-detail optimization
recommendation — it's a public binding conformance requirement. If review
finds a reflection hot path, unnecessary allocation/copy, thread
contention, or a hidden wait, that binding is considered non-compliant.

## Substrate vs Public Binding Surface

A bindings implementation sits on top of the helper substrate C API core
provides (the `*_part` family). The public API exposed to a bindings user
does not have to follow that helper's signature shape. What is fixed by
the rule below is which core functions the internal implementation is
allowed to call.

This document interprets the following boundary:

- The `*_part` helper substrate contract in `core/include/zlink.h` is the
  native substrate a bindings implementation must use.
- A document under `doc/spec/bindings/` defines only the
  **public convenience contract** each language binding provides
  externally.

In other words, the binding's public API can look different from the
helper substrate. But how it calls core internally must not differ.

For example, the following structure is required.

- The core substrate has a primitive surface such as `*_part`,
  `has_more`, and a caller-provided `zlink_msg_t`.
- Java, `.NET`, `Go`, `Rust`, `Python`, `Node`, `C++`, and C bindings
  layer a language-friendly public API on top — `Received`, `Message`,
  collections, and request/reply convenience.
- Any path inside the public API that calls core directly must use the
  `*_part` substrate. It must not call an aggregate-shaped core function
  (`zlink_send`, `zlink_recv`, `zlink_publish`, and so on) from inside a
  binding.

The following conditions must always hold:

- A binding's public API semantic contract must be explainable in terms
  of the core contract.
- A binding must not directly expose a low-level detail that exists only
  in the helper substrate.
- A binding must not expose part-by-part receive as a public binding
  API, such as `RecvPart`, `RecvRoutedPart`, `SubscribePart`,
  `recv_part`, `recv_routed_part`, or `subscribe_part`. The binding
  runtime absorbs the part loop, `has_more`, and per-part envelope
  metadata into an aggregate result storage internally.
- A document under `doc/spec/bindings/` does not document the helper
  substrate signature itself as a public contract.
- The helper substrate is treated only as a foundation layer for
  bindings implementation and performance optimization.

In other words, the bindings policy documents are governed not by "what
the helper looks like" but by "what public contract a binding user
ultimately sees."

## `*_part` Substrate Usage Requirement (Required)

The internal implementation of the send, request, reply, publish, and
subscribe function families must use core's `*_part` helper substrate.
This is a `Required` rule.

### Scope

This applies to every binding-internal implementation path in the
following families.

- send (including single-part, multi-part, and routed)
- recv (including single-part, multi-part, and routed)
- request (including dealer, router, and SPOT variants)
- reply (including router and SPOT variants)
- publish
- subscribe (including SPOT subscribe)

### Reason

Back when core provided both aggregate functions and the `*_part`
substrate, calling the aggregate function directly was allowed. But that
structure creates the following cost:

- Core first builds a native aggregate (a parts array).
- The binding then converts that aggregate again into a language-specific
  object (`Message[]`, `Received`, a value object).
- The result is a back-to-back "build the native aggregate → build the
  language-object aggregate" sequence, and this double-conversion cost
  becomes a real bottleneck on the hot path.

Using the `*_part` substrate directly lets a binding convert each part
straight into a language object one at a time, eliminating the native
aggregate-construction step entirely. This produces a measurable
performance difference especially in languages like Java and .NET, where
object materialization is expensive.

This rule is not for the sake of structural tidiness — it is a
requirement meant to **substantially reduce runtime performance cost**.

### The Public API Shape Stays The Same

This rule is about the internal implementation foundation. The public API
shape a binding user sees stays whatever each language's spec defines,
regardless of this rule.

- A user still uses a language-friendly API such as
  `send(List<Message>)`, `recv()`, or `request(...)`.
- The `*_part` call sequence is a binding-internal implementation detail
  and is not exposed to the user.
- A public binding's receive surface offers only an aggregate
  result-storage API such as `recv`, `subscribe`, or `recvRouted`. The
  `RecvPart`/`SubscribePart` family is a name for the performance
  optimization substrate, not a public contract name.

## Spot Get-Or-Create Mapping

Core provides `zlink_spot_node_spot_get_or_new(...)` for the atomic
"get a local logical Spot by routing id, or create it if absent"
contract.

Every higher-level binding must map its public get-or-create SpotNode API
directly onto that C function. It must not compose `spot_lookup()` and
`create_spot()` to emulate the same behavior, because doing so loses
core's atomicity contract and reintroduces the lookup/create race.

The per-language names are:

- C++: `spot_node_t::get_or_create_spot(...)`
- .NET binding: `SpotNode.GetOrCreateSpot(...)`
- Java: `SpotNode.getOrCreateSpot(...)`
- Node: `SpotNode.getOrCreateSpot(...)`
- Go: `SpotNode.GetOrCreateSpot(...)`
- Rust: `SpotNode::get_or_create_spot(...)`
- Python: `SpotNode.get_or_create_spot(...)`

Each wrapper returns both the owned `Spot` facade and whether this call
created the logical spot. The returned facade follows that language's
normal Spot lifetime rules.

### Compliance Check

Confirm the following during implementation review and verification.

- No path in the binding source directly calls an aggregate symbol
  (`zlink_send`, `zlink_recv`, `zlink_send_rid`, `zlink_publish`,
  `zlink_subscribe`, `zlink_router_recv`, `zlink_dealer_request`,
  `zlink_router_request`, `zlink_router_reply`, `zlink_spot_send_*`,
  `zlink_spot_request_*`, `zlink_spot_reply_*`, `zlink_spot_subscribe`,
  and so on).
- The matching `*_part` symbol is used instead
  (`zlink_send_part`, `zlink_recv_part`, `zlink_send_part_rid`,
  `zlink_publish_part`, `zlink_subscribe_part`, `zlink_router_recv_part`,
  `zlink_dealer_request_part`, `zlink_router_request_part`,
  `zlink_router_reply_part`, `zlink_spot_*_part`, and so on).
- Non-compliance blocks the review.

## Public vs Internal API Boundary

Every binding must separate the public contract from the internal
implementation surface. This document and each per-language README
define the public API's boundary and the library's shape. The exact
function, method, and type list is owned by the public contract source
that each wrapper binding's per-language README designates, except for
C. For C++ and .NET, that location is a literal `Contracts/` folder; for
Java, Node, Python, Go, and Rust, it's the package, module, or export
surface each README designates. The installed header, package
entrypoint, `.d.ts`, `__init__.py`, and `lib.rs` re-export are the
projections that expose this contract to the user. C is the exception —
`core/include/zlink.h` is the single baseline for the public C ABI.

The following principles apply to every binding in common.

- Any type, function, method, module, package, or namespace not included
  in the per-language public contract source is treated as internal
  implementation detail.
- A per-language README does not repeat every public member. Instead it
  defines the public contract source location, source layout, API-change
  procedure, runtime/internal boundary, and performance policy.
- An internal API is not enough to merely look internal by name. Where a
  language supports it, use a language-native boundary — package export,
  module export, assembly visibility, crate re-export, package `exports`,
  an `internal/` directory — to actually restrict access.
- In principle, perf, sample, and test code must also use only the public
  binding entrypoint. Being in the same repository does not license a
  direct import of or reference to an internal helper.
- Public contract verification is judged against the entrypoint a
  deployed binding consumer actually sees. The mere existence of an
  internal symbol inside the source tree does not make it public.
- A binding that ships an installed header alongside a compiled binding
  library, like C++, keeps a public `Contracts/` inside the installed
  `include/` tree, and hides the implementation as private files under
  `bindings/cpp/src/Runtime/`. An aggregate header may still exist, but it
  must not become the only entry point for finding a public class.
- The freedom to refactor internal structure is guaranteed, but only
  within the scope that preserves the public contract.

In other words, this document's purpose is not only to define the public
API boundary and library shape — it also includes enforcing that boundary
so a non-public API cannot be used as if it were public.

### Per-Language Contract/Runtime Separation

Every wrapper binding except C must separate the public contract from the
runtime implementation. However, how it separates them must follow that
language's package, module, and import-path rules. A per-language README
must specify both the actual repository path and the actual
package/module path. `Contracts` and `Runtime` are shared logical
category names — they do not mean every language must turn that literal
word into a public package or import path.
C++ is a C++20 binding; its public contract root is
`bindings/cpp/include/zlink/Contracts/` and its runtime implementation
root is `bindings/cpp/src/Runtime/`.
Because a Java package path is itself the source folder, Java reveals the
role structure through lower-case Java packages such as
`systems.zlink.contracts.*` and `systems.zlink.runtime.*`.
Languages such as Go, Rust, and Python, where the folder path connects
directly to the package/module/import path, also separate the public
contract from the runtime implementation inside the actual
package/module tree.
For Node/TypeScript, `package.json` exports set the public boundary, but
because the source folder name can also cause deep-import confusion, it
follows the actual source path and package-export rules its per-language
README specifies.

C is the native C ABI baseline. C's public contract is
`core/include/zlink.h`, and `bindings/c` aligns its sample, test, perf,
packaging, and any necessary mapping policy against that C API. C is not
forced to have a separate `Contracts/`/`Runtime/` layering.

`Contracts` is the role for the public contract source a user must check.
`Runtime` is the role for implementation detail such as a native handle,
callback bridge, request progress pump, helper substrate call, or object
lifetime correction. `Native` is the role reserved for the native
bridge — FFI, P/Invoke, JNI/Panama, N-API, cgo. In a language the
document names explicitly, such as C++ and .NET, these role names are
used as the actual folder names; in other languages, the same role is
expressed through the package/module/export structure the per-language
README specifies.

`Contracts` and `Runtime` are shared role names. That does not mean they
are the public package, namespace, module, or import-path name. A
language where directory structure directly affects the package/module
path does not expose `Contracts` or `Runtime` as a public import path.
Instead, it places the contract at an actual path inside the public
package/module tree, and keeps the runtime implementation inside a
language-specific private boundary such as `internal`, a private module,
an unexported module, or a `pub(crate)` module.

#### Contract / Runtime Placement Rules

The following criteria apply to every wrapper binding except C. Check
this table first when adding a new public API or moving an
implementation.

| Item | Location |
|---|---|
| A public behavior contract a user calls or references by type | The matching category in public contract source |
| The contract for a public constructor, factory, or builder start point | The matching category in public contract source |
| A public free function, static facade, extension helper, or module function | The matching category in public contract source |
| A public builder convenience method or helper | The matching category in public contract source |
| A DTO, value object, enum, or public error/result type | The matching category in public contract source |
| A runtime concrete class, socket kernel, or handle owner | The matching category in runtime/internal source |
| A request progress pump, callback trampoline, or part-loop helper | The matching category in runtime/internal source |
| A native handle wrapper, FFI declaration, struct mirror, or marshalling helper | Native bridge source |
| Generated native loading code, platform artifact lookup | Native bridge source |

The judgment rules are:

- A public contract type's public signature does not reference a native
  bridge type.
- If runtime/internal source needs a user-facing method, add the contract
  to public contract source first. The runtime implementation implements
  or projects that contract.
- If a helper a user calls directly is public — whether it's shaped as a
  class method, static method, free function, extension method, or
  module function — put the contract in public contract source. Do not
  leave it in a runtime-only location just because it's a simple
  convenience function.
- A public factory may return a runtime concrete type. But its
  construction behavior and the user-observable behavior of the returned
  type must be explainable in public contract source.
- Do not expose the runtime/internal folder name or the module/package
  path itself as public API. However, a basic implementation class or
  type such as `Context`, socket, `SpotNode`, `Poller`, or `Timer` may be
  exposed as a per-language public projection. In that case, the public
  behavior a user observes must still be explainable in public contract
  source.
- A public contract type's public signature does not reference a native
  bridge type. Even when a concrete value object's internals must use
  native-backed storage, keep the P/Invoke/JNI/N-API/cgo declaration and
  the marshalling-only struct mirror in native bridge source.
- Keep a value-only DTO/value/enum/error/result type concrete. Do not
  wrap it in a meaningless interface, trait, or protocol for the sake of
  symmetry.

The fixed categories are:

- `Core/`: context, version, roles, and utility resources.
- `Messaging/`: message, routing id, received, topic message, multipart.
- `Sockets/`: socket contracts, socket implementations, socket options.
- `Eventing/`: monitor, poller, timer, readiness events.
- `Service/`: SPOT, actor, SPOT topology.
- `Errors/`: public error/result/exception domains and runtime mapping.
- `Native/`: the native bridge category, kept only under runtime/internal
  source.

These category names are fixed in the documentation and review standard.
The actual file and folder names follow the convention each per-language
README specifies. If a new category is needed, change this shared policy
together with the per-language README structure (except C) before using
it. Do not create a `Native` category in public contract source — the
native bridge always lives under runtime/internal source.

The wrapper binding's shared structure is fixed to the following role
structure. Except for C, each per-language README must show this
structure again using that language's actual repository path and
package/module/import path, and the implementation must match that
structure.

```text
bindings/<lang>/
+-- <public-package-or-module-root>/
|   +-- <public contract categories>
|   |   +-- Core
|   |   +-- Messaging
|   |   +-- Sockets
|   |   +-- Eventing
|   |   +-- Service
|   |   +-- Errors
|   +-- <private runtime/internal area>
|   |   +-- Core
|   |   +-- Messaging
|   |   +-- Sockets
|   |   +-- Eventing
|   |   +-- Service
|   |   +-- Errors
|   |   +-- Native
+-- codecs/
+-- tests/
+-- samples/
+-- perf/
+-- native/
+-- runtimes/
```

The shared standard is:

- The public API contract a user must check should be gathered in an
  easy-to-find location.
- Implementation detail such as a native handle, callback bridge, request
  progress pump, helper substrate call, or object lifetime correction
  must not mix with the public contract.
- Keep a DTO, value object, enum, or error/result object as a concrete
  type. Do not wrap a value-only type in a meaningless interface or
  trait.
- A type that hides a native resource and its behavior — socket, context,
  monitor, timer, service node, spot, actor — may have an abstraction
  boundary that fits the language's convention.
- In principle, write perf, sample, and framework adapters against the
  public contract too. Depending on a runtime-internal type just because
  it's in the same repository weakens the public/internal boundary.

The per-language application direction is:

| Binding | Application standard |
|---|---|
| C | `core/include/zlink.h` is the single baseline for the public C ABI. `bindings/c` does not add a separate contract/runtime layer, and aligns only the C-API-based mapping, sample, test, perf, and packaging policy. |
| C++ | `bindings/cpp/include/zlink/Contracts/` is the public C++ contract location. `bindings/cpp/src/Runtime/` is the private implementation location. It prefers C++20, RAII classes, and concrete values, and does not over-wrap a public class in a virtual interface. |
| .NET | Detailed standards follow the [.NET binding blueprint](dotnet/README.md). This document does not duplicate .NET's detailed file structure. |
| Java | The public contract package under `bindings/java/src/main/java/systems/zlink/contracts/` is the public contract location. Because Java follows URL-based package layout, it reflects the lower-case `contracts` and `runtime` packages in the actual folders. The native bridge lives under the non-exported `systems.zlink.runtime.nativeapi`. |
| Node | `bindings/node/src/index.ts` and the `package.json` exports are the public contract projection. Contract source lives at a lower-case source path such as `bindings/node/src/zlink/contracts/`, and the runtime/native addon implementation is hidden under `bindings/node/src/zlink/runtime/`. |
| Python | `bindings/python/src/zlink/contracts/` is the public contract source. The `zlink` root package is the projection that re-exports this contract, and the native/FFI implementation lives under private packages such as `_runtime/` and `_native/`. |
| Go | The `bindings/go/contracts/` public package is the Go public contract source. Currently the runtime/native implementation is owned by unexported implementation files at the root and by cgo bridge files. If it is split into a separate package later, it should be hidden under Go's `internal/` convention. |
| Rust | `bindings/rust/src/contracts/` serves as the public contract source. `lib.rs` re-exports the necessary types as a crate-root/domain projection, and `bindings/rust/src/runtime/` and `bindings/rust/src/runtime/native/` stay private modules. |

A review is not judged by simply "does an interface exist," but by the
following questions.

- Can a user understand the usable API just by looking at the public
  contract?
- Does the public contract avoid directly requiring a runtime concrete
  type, native handle, or helper bridge type?
- Does the file hold an independent concept, or a group that shares the
  same reason to change?
- Could a thin file that holds only a marker, delegate, small enum, or
  one-line record be merged into a nearby contract file instead?
- Has abstracting a value type blurred equality, ownership, or the cost
  model instead of helping?
- Does it use the language ecosystem's natural encapsulation mechanism?

#### Per-Binding Target Physical Layout

Each per-language README treats the path and role below as the target for
new alignment work. If the current implementation still differs from
this structure, align it in stages together with that binding's API,
sample, and perf work during its structural cleanup. This does not mean
the public package, namespace, module, or import path directly exposes
the `Contracts` or `Runtime` name below.

| Binding | Contract root | Runtime root | Public projection |
|---|---|---|---|
| C++ | `bindings/cpp/include/zlink/Contracts/` | `bindings/cpp/src/Runtime/` | `#include <zlink.hpp>` and installed `include/zlink/...` headers |
| .NET | See [dotnet/README.md](dotnet/README.md) | See [dotnet/README.md](dotnet/README.md) | See [dotnet/README.md](dotnet/README.md) |
| Java | `bindings/java/src/main/java/systems/zlink/contracts/` | `bindings/java/src/main/java/systems/zlink/runtime/` | exported `systems.zlink.contracts.*` JPMS packages and Maven artifact |
| Node | `bindings/node/src/index.ts` and `bindings/node/src/zlink/contracts/` | `bindings/node/src/zlink/runtime/` | package root export, generated `.d.ts`, and `package.json` exports |
| Python | `bindings/python/src/zlink/contracts/` | `bindings/python/src/zlink/_runtime/` and `bindings/python/src/zlink/_native/` | `zlink` package exports from `__init__.py` |
| Go | `bindings/go/contracts/` public package | current root unexported implementation files and cgo bridge files; future split should use `bindings/go/internal/...` | exported identifiers in `zlink.systems/zlink/contracts` |
| Rust | `bindings/rust/src/contracts/` | private `bindings/rust/src/runtime/` and `bindings/rust/src/runtime/native/` modules | `lib.rs` re-exports and public rustdoc projection |

Each per-language README must show where the `Core`, `Messaging`,
`Sockets`, `Eventing`, `Service`, and `Errors` roles are actually placed
in its source. `Native` exists only as a runtime/native bridge role and
is never made a public contract role.

### Package / Namespace Identity Policy

The official library domain is `zlink.systems`. Any per-language package,
namespace, module, or artifact name being newly fixed or changed must
start from this domain, and must not put a prior organization name or a
repository owner's name into a canonical public identifier.

| Binding | Canonical public identity |
|---|---|
| C | The public header is `zlink.h`; the symbol prefix is `zlink_` |
| C++ | The namespace is `zlink`; the installed header root is `include/zlink/` |
| .NET | The NuGet package id and root namespace are `Systems.Zlink` |
| Java | The Maven group id, JPMS module, and root package are `systems.zlink` |
| Node | The npm package is `@zlink-systems/zlink`; the public entrypoint is the package root |
| Python | The distribution name and import package are `zlink` |
| Go | The module path is `zlink.systems/zlink`; the public package is `zlink` |
| Rust | The crate name and public crate root are `zlink` |

- A framework extension package and namespace stays under that
  framework language's canonical identity. For example, `.NET` uses
  `Zlink.Framework.*`, and Java uses `systems.zlink.framework.*`.
- Go, Python, and Rust are not currently framework targets, so they do
  not add a binding-owned codec module.
- A Node extension package's name follows ecosystem convention, but its
  public identity must not drift outside the `zlink` and
  `zlink.systems` domain.
- New documents, samples, and tests use only the canonical identity.
- Even if an old `Zlink` root namespace or package id remains for
  implementation compatibility, it is not the canonical public identity,
  and no new public API is added under it.

### Core Interface Shape Rules

This section summarizes the required public interface shape for every
wrapper binding except C. See the recv section and operation builder
section further below for the detailed contract. C keeps the functional
ABI of `core/include/zlink.h` as-is, so this wrapper rule does not apply
to it.

- The data-plane `recv` and `subscribe` families take caller-provided
  output storage. The caller creates a result object such as `Received`,
  `TopicMessage`, or `SubscriptionEvent`, and the binding updates that
  object.
- A data-plane receive's return value expresses only "was data
  received." A hard error is delivered as a typed exception, `error`, or
  `Result`, per language convention.
- A control-plane API such as `Monitor.recv` or `Timer.recv` is called
  infrequently and returns a small result, so a per-language nullable,
  optional, or value-return form is allowed.
- A service control/admission receive such as `Spot.recvActorJoin` is
  also not a data-plane drain path, so a per-language nullable, optional,
  or result-value form is allowed. However, no-data and a hard error must
  still be separated, and the public contract must clearly document this
  exception.
- `send`, routed send, `publish`, `request`, `reply`, SPOT
  send/request/reply, and the Actor location/session-attach family return
  an operation builder.
- A builder start point's arguments take only the operation's target —
  destination, topic, channel, routing id, or request sequence. Payload,
  flags, timeout, callback, and the async/callback submit choice are
  expressed at the builder stage.
- Multipart payload accumulates through repeated `message(...)` calls on
  the builder. A `messages(...)` convenience may exist per language
  convention, but the canonical path is the builder. If such a
  convenience is public, it is part of the builder contract and belongs
  in `Contracts/`.
- Do not multiply operation-start names such as `sendNoWait`,
  `publishWithFlags`, `requestAsync`, or `requestCallback`. Keep the same
  operation name, and let the builder stage absorb the variation. The
  per-language final execution method for an async or callback completion
  surface follows the
  [bindings async execution surface policy](async-coroutine-policy.md).
- Resource creation is not scattered across public constructors on
  several runtime classes. A per-binding root facade or context factory
  owns construction responsibility. For example, the .NET binding
  creates a context with `Zlink.CreateContext()`, and creates socket and
  service resources through `IContext.Create...` factories.
- A runtime concrete type must not appear directly in a public contract
  signature. A public method's arguments and return value must be
  explainable through a contract interface, value object, DTO, enum, or
  result/error type.
- Samples, perf, and framework adapters use only this canonical
  interface. Do not write new code against a runtime-internal helper or a
  legacy overload.

### The Send/Recv Public Shape Is Fixed

The public `send`/`recv` shape of the bindings is not something to
redecide every time the substrate helper's shape changes. It is fixed to
the public shape this document and each per-language binding spec
define.

In other words, even if the helper substrate's shape changes — `*_part`,
`has_more`, caller-provided message storage — the binding's public API
must keep the following principles.

- A binding user sees the `send`, `recv`, request/reply, and callback
  shape defined in the language document.
- Multipart can continue to be offered through whatever aggregate
  convenience model each language document defines.
- A binding's public `send`/`recv` shape must not be shaken up just
  because the helper substrate changed.
- Changing the public shape must be treated as a public API change
  separate from introducing the helper, and the `doc/spec/bindings/`
  document must be updated first.

In other words, even if a helper C API is introduced going forward, a
binding's `send`/`recv` is "the implementation foundation changing," not
"the shape the user sees changing automatically."

### Canonical Recv: Caller-Provided Storage

For a high-level binding (C++ / .NET / Java / Node / Python / Go / Rust),
the data-plane recv surface's canonical form is a **ref-out shape that
takes a caller-pre-built result storage as a parameter and updates its
internal state**. A shape that allocates and returns a new result
instance on every call forces hot-path allocation overhead, so it is not
used as the canonical surface.

This rule is `Required`. When building a new binding or updating an
existing one, the canonical recv surface must satisfy this section.

#### Scope (all data-plane recv)

| Surface | Result type (caller storage) |
|---|---|
| `MessageSocketBase.recv` (PAIR / DEALER) | `Received` |
| `RoutedMessageSocketBase.recv` (ROUTER) | `Received` |
| `StreamSocket.recv` | `Received` |
| `SubscriberSocketBase.subscribe` (SUB / XSUB) | `TopicMessage` |
| `XPubSocketBase.receiveSubscriptionEvent` | `SubscriptionEvent` |
| `Spot.subscribe` | `TopicMessage` |
| `Spot.recv` (routed) | `Received` |

`Monitor.recv` (`MonitorEvent`) and `Timer.recv` (`uint64`) are
control-plane calls, called infrequently and with a lightweight value
result, so they are not in scope for this section. They keep a
return-form (or a per-language `Optional`/nullable/`Option`). A service
control-plane API that receives an Actor join admission request, such as
`Spot.recvActorJoin`, can apply the same exception. In that case, the
public contract must document the no-data representation and the hard
error representation separately.

#### Base Contract

- The `recv` caller pre-builds a long-lived result storage and passes the
  same instance on every call. The binding reuses its internal part
  collection, routing-id storage, and topic buffer as much as possible,
  driving per-recv allocation toward zero.
- The return value carries only "was something received" — a boolean, or
  an equivalent representation that distinguishes success from no-data.
  A hard error is delivered as an exception or error code, per language
  convention.
- When a call with a non-blocking flag such as `recv_flags_t::dontwait`
  finds no data, it returns a no-data representation used together with
  caller-provided storage, such as `false`, `recv_result_t::no_data`,
  `(false, nil)`, or `Ok(false)`. It does not signal EAGAIN as an
  exception.
- A multipart result accumulates into the caller's result storage. The
  binding must not build a temporary collection and cache it separately
  from the caller's result storage — that allocation would not go away.
- For routed recv (router / spot), the routing id must be filled into
  storage inside the caller-provided `Received`. A path that allocates a
  new byte array per routing id does not belong on the internal hot
  path.

#### Canonical Per-Language Signature

Apply the same ref-out pattern to each surface in the table above. Below
are examples keyed on the `Received` result type; `TopicMessage` and
`SubscriptionEvent` follow the same pattern.

| Binding | Canonical signature |
|---|---|
| C++ | `int recv(received_t& out, recv_flags_t flags = recv_flags_t::none);` 0 = success; failure or no data returns a `recv_result_t` integer value. If a local failure such as message initialization happens inside the binding, it returns -1 and sets errno. Multipart results fill `out.parts`. `subscribe(topic_message_t& out, int flags)` and `receive_subscription_event(subscription_event_t& out, int flags)` follow the same rule. |
| .NET | `bool Recv(Received result, RecvFlags flags = RecvFlags.None);` `bool Subscribe(TopicMessage result, RecvFlags flags = RecvFlags.None);` `bool ReceiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags = RecvFlags.None);` A `Received` storage is created with `Received.Create()`. true = received, false = no data (DontWait). A hard error is `ZlinkException`. |
| Java | `boolean recv(Received result, RecvFlags flags);` `boolean subscribe(TopicMessage result, RecvFlags flags);` `boolean receiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags);` |
| Node | `recv(received: Received, flags?: RecvFlag): boolean;` `subscribe(topic: TopicMessage, flags?: RecvFlag): boolean;` `receiveSubscriptionEvent(event: SubscriptionEvent, flags?: RecvFlag): boolean;` |
| Python | `def recv_into(self, received: Received, *, flags: int = 0) -> bool: ...` `def subscribe_into(self, topic: TopicMessage, *, flags: int = 0) -> bool: ...` `def receive_subscription_event_into(self, event: SubscriptionEvent, *, flags: int = 0) -> bool: ...` |
| Go | `func (s *Socket) Recv(out *Received, flags RecvFlags) (bool, error)` `func (s *Socket) Subscribe(out *TopicMessage, flags RecvFlags) (bool, error)` `func (s *Socket) ReceiveSubscriptionEvent(out *SubscriptionEvent, flags RecvFlags) (bool, error)` |
| Rust | `pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError>;` `pub fn subscribe(&self, out: &mut TopicMessage, flags: RecvFlags) -> Result<bool, RecvError>;` `pub fn receive_subscription_event(&self, out: &mut SubscriptionEvent, flags: RecvFlags) -> Result<bool, RecvError>;` |

A C ABI binding is not in scope for this section. The C binding exposes
`zlink.h`'s typed substrate (`zlink_router_recv_part`,
`zlink_subscribe_part`, and so on) as-is.

#### Unifying `Received` Envelope Meaning

For a high-level binding (C++ / .NET / Java / Node / Python / Go / Rust),
`Received` is a **shared envelope that holds the result of one
data-plane recv call**. The meaning of request, reply, routed source, and
payload lifecycle must stay the same regardless of socket kind or
service kind.

The rules below are `Required`.

- The receive results of PAIR / DEALER / ROUTER / STREAM / SPOT routed
  recv all use the same `Received` meaning.
- A request-reply receive result must not fork into a separate
  protocol-specific result type. A surface that splits request meaning by
  socket kind into separate public types — `DealerReceived`,
  `RouterReceived`, `SpotReceived` — is not canonical.
- Request meaning is independent of socket kind. If `request_seq` is
  present, the receive result has request-reply context; if not, it's an
  ordinary receive result.
- The reply target, send-back target, and source routing metadata are
  encapsulated inside `Received`'s own context. A user must not need to
  know a socket-kind-specific frame format or internal dispatch rule to
  handle a request.
- Per-language names and optional representations (`null`, `None`,
  `Optional`, `Option`, a zero value plus a `has` flag, and so on) can
  differ, but the canonical field/method meaning must match the
  [Domain Object Canonical Shape](#domain-object-canonical-shape-shared-by-every-binding)
  section.

The C ABI binding is an exception. C does not build a managed/object
result storage; it exposes the same envelope components through typed
out-params such as `zlink_router_recv_part()`, `zlink_spot_recv_part()`,
and `zlink_dealer_recv_part()`. Do not add a public aggregate object such
as `zlink_received_t` to C — doing so would grow message-part ownership,
init/close/reset, and reply-context retention into a new public lifetime
contract. If a C helper is needed, keep it only as a sample/perf/internal
helper.

A per-language detail document may separately list a deprecated overload
kept for backward compatibility. The table above lists only the
canonical path new code and samples/perf must follow.

#### Result-Storage Reuse Contract

- A result storage (Received / TopicMessage / SubscriptionEvent)
  automatically resets its internal state before receiving a new recv
  result. Passing the same instance to `recv` repeatedly is normal usage.
- .NET's `Received` does not expose a public constructor. A caller uses
  `Received.Create()` to build the storage to be filled. `Received` is a
  concrete receive buffer and is not split into a separate read
  interface.
- If a caller calls the next recv without separately `move`-ing the
  previous recv's part messages, the previous message must be closed
  appropriately. The binding provides a separate helper (such as
  `takeFirstPart`) that hands ownership of a part `Message` to the
  caller.
- Thread safety does not guarantee that multiple threads can pass the
  same result storage into recv concurrently. The existing policy that a
  socket is recv'd by a single thread still holds.

### Operation Builder Policy

zlink's send/request/reply/publish family, and the Actor
location/session-attach family, all have many combination axes. Spreading
target path, payload part count, `flags`, `timeout`, and the
async/callback completion mode across plain method overloads makes a
socket or service handle a shallow, wide interface, and forces multipart
payload to be wrapped in an external List/Vector container. A high-level
binding hides this combinatorial complexity inside an operation object,
and multipart naturally accumulates through repeated `message(...)` calls
on the builder.

This policy does not apply to the C ABI binding. The C binding keeps the
functional contract that matches `zlink.h`. It applies to the canonical
public API of high-level bindings such as C++ / Java / .NET / Node /
Python / Go / Rust.

#### Start Points In Scope

An operation builder start point is exposed with the same pattern across
**every send, request, reply, publish, Actor location, and Actor
session-attach surface**. The name is converted to fit language
convention.

##### Spot facade (`Spot` / `spot_t`)

- `publish(topic)`
- `sendToChannel(channelName)` / `send_to_channel(channel_name)`
- `sendToSpot(destNodeRid, destSpotRid)` / `send_to_spot(...)`
- `requestToChannel(channelName)` / `request_to_channel(...)`
- `requestToSpot(destNodeRid, destSpotRid)` / `request_to_spot(...)`
- `requestToRouter(peerRid)` / `request_to_router(...)`
- `replyToSpot(destNodeRid, destSpotRid, requestSeq)` / `reply_to_spot(...)`
- `replyToRouter(peerRid, requestSeq)` / `reply_to_router(...)`
- `replyActorJoin(request, accepted)` (Actor join admission reply)

##### Raw socket facade

- `PubSocket.publish(topic)` / `XPubSocket.publish(topic)`
- `DealerSocket.send()` / `DealerSocket.request()`
- `RouterSocket.send(rid)` / `RouterSocket.request(rid)` / `RouterSocket.reply(rid, requestSeq)`
- `RouterSocket.sendToSpot(destNodeRid, destSpotRid)` / `requestToSpot(...)` /
  `replyToSpot(destNodeRid, destSpotRid, requestSeq)`
- `PairSocket.send()` (PAIR send)
- `StreamSocket.sendTo(rid)` (STREAM peer send)
- Any other raw send-capable socket's send entrypoint exposes an
  operation builder start point the same way.

##### SpotNode/StreamSocket Actor surface

- `SpotNode.joinActor(actor, destNodeRid, destSpotRid)` / `join_actor(...)`
- `SpotNode.leaveActor(actor, currentSpotRid)` / `leave_actor(...)`
- `SpotNode.destroyActor(actor)` / `destroy_actor(...)`
- `SpotNode.remoteActorGetRef(targetNodeRid, actorId)` / `remote_actor_get_ref(...)`
- `StreamSocket.bindActor(sessionRid, actor)` / `bind_actor(...)`
- `StreamSocket.unbindActor(sessionRid, actorId)` / `unbind_actor(...)`
- `StreamSocket.sendBoundActor(sessionRid, actorId)` / `send_bound_actor(...)`
- `SpotNode.sendBoundSessionMsg(actor)` / `send_bound_session_msg(...)`

#### Common Builder Rules

- A start point does not send immediately — it returns a per-language
  operation builder such as `SendOp`, `RequestOp`, `ReplyOp`,
  `ActorJoinOp`, `ActorLeaveOp`, `ActorDestroyOp`, `ActorLookupOp`,
  `ActorBindOp`, or `ActorUnbindOp`. Regardless of which start point is
  used, multipart payload is always expressed through repeated
  `.message(...)` calls.
- A builder convenience such as `.messages(...)`, `.flags(...)`,
  `.timeout(...)`, a callback submit, or the final execution method of an
  async completion is part of the builder contract if it is public. It
  must not be defined only as a runtime-internal shortcut. The
  per-language name and meaning of the async-completion final execution
  method belongs in the
  [bindings async execution surface policy](async-coroutine-policy.md).
- Payload accumulates through repeated `message(part)` calls on the
  builder. A single payload and a multipart payload are not split into
  separate start-point overloads. Multipart is not wrapped in an
  external List/Vector container.
- Do not create a single-payload shortcut overload with the same name as
  a start point. For example, public overloads such as `send(message)`,
  `send(routingId, message)`, `publish(topic, message)`,
  `sendToChannel(channelName, message)`, and
  `sendToSpot(nodeRid, spotRid, message)` are forbidden. Express all of
  these through builder steps, such as
  `send(...).message(message).submit()`.
- When a language can naturally express an explicit move/consume name, it
  can add an ownership-transfer step (`moveMessage`, `MoveMessage`,
  `move_message`, and so on) inside the same builder. This step is not a
  new operation start point, and the name and documentation must clearly
  state that the caller cannot reuse that message even after a submit
  failure. This does not change the existing `message(...)` step's
  contract of preserving the original on failure.
- For an operation where payload is semantically required — send,
  request, reply, publish, Actor join, ActorReplyJoin, and so on — a
  `submit` with zero messages is forbidden. A language whose type system
  can prevent this blocks it at compile time; other languages block it
  with a validation error at `submit` time.
- For an operation with no payload — Actor `leave`, `destroy`,
  `bindActor`, `unbindActor`, `remoteActorGetRef` — the builder can submit
  immediately without a `message(...)` step. But it still exposes the
  same builder shape and option steps (`flags(...)`, `timeout(...)`,
  `callback(...)`, the async-completion final execution method).
- `flags`, `timeout`, and the callback/async choice are optional builder
  steps, not start-point parameters. A start point takes only
  semantically key arguments, such as the target address or request
  sequence.
- A messaging call in a sample or documentation example does not repeat a
  default value. For a message-sending function such as `request`,
  `requestToChannel`, `send`, `sendToChannel`, `reply`, or `publish`, use
  the packet name inferred by default from the request object or the
  registered packet type. Use a packet-name override such as
  `.packetName(...)`, `.packet_name(...)`, or `.PacketName(...)` only when
  the packet actually being sent differs from the request type's default
  packet name. The same way, use a per-call timeout such as
  `.timeout(...)` or `.Timeout(...)` only when that call needs a value
  different from the default timeout configured on the socket or
  framework. This rule is not about making an example look shorter — it's
  a sample contract that keeps a user from mistaking an unnecessary
  option for standard usage.
- A helper that directly builds a request/reply protocol envelope does
  not belong on the public binding surface. An API such as
  `requestFrame(...)` exposes the request sequence and frame layout to
  the caller, so it must stay a runtime/internal helper.
- A reply must start from the received request context. The public
  binding API offers only a surface that reveals a reply-capable
  context, such as `received.reply()` or
  `router.reply(peerRid, requestSeq)`. An API such as
  `dealer.reply(requestToken, parts)`, where DEALER starts a reply with
  an arbitrary token, does not belong on the public binding surface. A
  DEALER cannot designate a specific peer routing id, so reply-routing
  decisions would leak into a protocol helper, and the user would need to
  understand token semantics.
- An async request or async Actor operation does not take submit flags. A
  callback form may take `flags` to express a non-blocking submit. The
  detailed difference in completion mode follows the
  [bindings async execution surface policy](async-coroutine-policy.md).
- A builder cannot be submitted again once it has been submitted. A
  language that offers a move-only or ownership type blocks this by
  type; otherwise it is blocked by a runtime state check.
- Because an Actor join start point's admission completion shape differs,
  its builder exposes a dedicated completion result (`ActorJoinResult`)
  that captures both the reply payload and the final Actor ref together.
  lookup/destroy/leave/bind/unbind use the ordinary reply completion
  shape (`RequestResult`).

#### Common Flow Example

Names are converted to fit language convention.

```java
spot.publish(topic)
    .message(part1)
    .message(part2)
    .flags(SendFlags.DONTWAIT)
    .submit();

routerSocket.requestToSpot(destNodeRid, destSpotRid)
    .message(reqPart)
    .timeout(Duration.ofSeconds(3))
    .submit();

spotNode.joinActor(actor, destNodeRid, destUserSpotRid)
    .message(joinStatePart)
    .timeout(Duration.ofSeconds(3))
    .submit(joinCallback);

streamSocket.bindActor(sessionRid, actorRef)
    .timeout(Duration.ofSeconds(2))
    .submit(replyCallback);
```

#### Per-Language Async Execution Surface Standard

The per-language final execution method for an async or callback
completion belongs in the
[bindings async execution surface policy](async-coroutine-policy.md).

This rule is Required under the POSD standard. When adding or cleaning up
a new send/request/reply/publish or Actor location/attach public API,
use this operation builder shape and the async execution surface policy
as the baseline, and do not grow the existing overloads into more
canonical APIs.

## Core Alignment Rules

This section is a summary of the core contract that takes priority over
the detail examples in per-language documents. If there is a mismatch
between `core/include/zlink.h` and a per-language document, this section
is the baseline.

#### Direct Receive Callback Constraints

- The direct receive callback install surface exists only for raw
  `STREAM` and SPOT routed receive.
- A binding must not publicly expose an `onReceive`-style direct data
  callback for raw `PAIR`, `DEALER`, or `ROUTER`.
- A binding must not publicly expose an `onSubscribe`-style direct topic
  callback for raw `SUB`, `XSUB`, or SPOT subscribe receive.
- `ROUTER` inbound routed traffic is received through a single routed
  recv surface. The binding runtime uses `zlink_router_recv_part()`
  internally, and exposes only the aggregate routed recv and the request
  completion callback on the public surface. It does not provide a
  direct receive callback.
- Core's raw `STREAM` is an exceptional type that picks one of three
  modes: `recv`, the raw callback (`zlink_recv_handler()`), or the packet
  callback (`zlink_stream_packet_handler()`). A high-level binding's
  canonical public contract exposes only the `recv` and packet-callback
  surfaces. The raw direct callback is used only as an internal binding
  primitive — adding it as public API requires changing this policy
  document and the matching language spec together first, splitting it
  out as a separate raw/low-level surface.

#### SPOT Channel And Dispatch Surface

- SPOT is a channel-aware model. A binding must provide
  `create_route_bridge(...)` or an equivalent typed bridge,
  `create_publisher(...)` or an equivalent publisher handle,
  `send_to_channel`, `send_to_spot`, `request_to_channel`, the
  channel-aware send/request operation builder start points, and the SPOT
  topic publish/subscribe surface. Legacy surfaces that attach an
  external channel `DEALER`, route mesh `ROUTER`, or raw `PUB` socket
  directly to a `SpotNode` are not part of the public contract.
- A SPOT subscribe result exposes topic/parts. The channel name is not
  repeated as a message result field.
- `zlink_spot_dispatch_event_handler()` is the canonical readable
  notification surface for the SPOT topic/routed/channel-reply/timer/actor
  planes.
- The Actor dispatch surface is a public service-layer capability, same
  as SPOT. Every binding exposes it through public types that fit its
  own language convention, and the shared meaning follows the
  `Actor Dispatch Binding Contract` section and the `Actor Dispatch
  Policy` section below.

#### Auto-HWM And SpotNode Options

- `ZLINK_CTX_OPT_AUTO_HWM_PROFILE` and
  `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` must be exposed by every
  binding as a typed context option. The profile value is one of
  compact, low latency, balanced, or throughput, and the default is
  balanced. A context message-unit default of `0` means using each
  socket type's default message unit.
- `MonitorStatus` must expose every one of core `zlink_monitor_status_t`'s
  auto-HWM v2 diagnostic fields without omission. Enabled, the profile
  enum, role, policy class, unit budget, size cap, socket message slots,
  effective message bytes, applied HWM, the recent recalculation reason
  enum, deferred shrink, and blocked ratio are all part of the public
  snapshot contract.
- A SPOT node option name follows core's public enum as-is. A binding
  does not expose a directional HWM option or a delivery-queue
  hard-limit option. What it exposes is the four admission options
  `ZLINK_SPOT_NODE_OPT_ROUTER_HWM_PROFILE`,
  `ZLINK_SPOT_NODE_OPT_ROUTER_HWM`,
  `ZLINK_SPOT_NODE_OPT_PUBSUB_HWM_PROFILE`,
  `ZLINK_SPOT_NODE_OPT_PUBSUB_HWM`, and the two dispatch-worker options
  `ZLINK_SPOT_NODE_OPT_DISPATCH_WORKERS_MIN` and
  `ZLINK_SPOT_NODE_OPT_DISPATCH_WORKERS_MAX`.
  The C API's shared `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES` stays only as an
  explicit override on a raw socket. A per-language high-level binding
  does not expose this value on a socket/SpotNode/Spot public facade — it
  exposes only the context option as the canonical API. A SPOT node or
  SPOT handle cannot set the raw socket's shared option; calling it fails
  with `EINVAL`. This value is not a message-size limit — it's the
  planning unit used to turn an auto-HWM budget into a slot count.
  A dispatch-worker option adjusts only the size of the callback worker
  pool owned by `SpotNode`, and does not mean `ZLINK_IO_THREADS` or a
  data-plane thread count. `min` must be at least 1, and `max` must be at
  least `min`. Absent explicit configuration, it maps to `min=max=1` when
  there is 1 CPU, and otherwise to `min=2`, `max=cpu_count`.
#### SPOT Status And Snapshot Names

- A SPOT binding status object must expose core's
  `disconnected_sub_target_count` and `disconnected_routed_target_count`
  under a name that fits language convention. Because core currently does
  not disconnect a target from delivery-queue growth alone, both values
  currently report `0`.
- When a SPOT binding exposes or documents an internal socket snapshot
  name, it uses the public snapshot name core returns as-is. The current
  names are `mesh-pub`, `mesh-xsub`, `peer_ctrl_pub`, `peer_ctrl_sub`,
  `routed-router`, `local-pub`, and `internal_receiver`. `local-pub` is
  the local fanout socket that sends to a subscriber inside the same
  node. (`ingress-sub`, `pub-ingress-tx`, `internal-router`, and
  `internal-router-tx` have been removed and are not part of the
  snapshot.)
#### Dispatch Readiness Meaning

- `zlink_spot_dispatch_event_handler()` is the single entry point for
  SPOT routed receive and Actor lifecycle readiness. A binding does not
  expose a direct routed callback as public API.
- `ZLINK_SPOT_DISPATCH_EVENT_SUBSCRIBE_READABLE` and
  `ZLINK_SPOT_DISPATCH_EVENT_ROUTED_READABLE` are readiness
  notifications, not message-count notifications. A binding must not
  describe or implement them as edge-triggered one-shot events.
- `ZLINK_SPOT_DISPATCH_EVENT_ACTOR_READABLE` and
  `ZLINK_SPOT_DISPATCH_EVENT_ACTOR_JOIN_READABLE` belong to the same
  dispatch-readiness axis. An Actor readable event must let the caller
  know which Actor to drain, and an Actor join readable event must be
  drained through `Spot`'s join receive surface.
- A SPOT dispatch consumer must reflect, in its documentation and
  samples, the rule of draining `subscribe`/`recv_routed` until each
  language's no-data representation appears. For example, C++ uses the
  `recv_result_t::no_data` return value, and Java/Node/Python use `false`
  from the API that fills caller-provided result storage.
- The first SPOT routed recv must not perform hidden activation, hidden
  queue open, or hidden target registration. A binding assumes the same
  premise and does not layer lazy bootstrap logic on top.
#### Send-Ready, Peer Weight, And STREAM Receive Modes

- `zlink_send_ready_handler()` and the poller's `ZLINK_POLLOUT` point to
  the same send-recovery readiness axis. A binding's documentation must
  describe them with the same meaning. `ZLINK_POLLOUT` is described as
  "send recovery readiness / backpressure recovery notification," not as
  "transport writable."
- A binding must expose the peer-weight surface as a per-language typed
  option/property. It applies to `ROUTER` and `DEALER`; the value range
  is `0..10000`, and the default is `100`. `0` means exclusion from new
  outbound selection. The matching submit failure code is
  `ZLINK_SUBMIT_NOT_ADMITTED` (value 13), and it must be included in
  every binding's `SubmitError` mapping.
- Core's raw `STREAM` can select only one of three receive modes at a
  time: (a) blocking/non-blocking recv based on `zlink_recv_part()`,
  (b) the raw direct callback `zlink_recv_handler()`, or (c) the packet
  callback `zlink_stream_packet_handler()`, which uses big-endian
  `u16 header_size + u32 body_size + header + body` framing. A second
  attach returns `EBUSY`. A high-level binding must keep the same
  mutual-exclusion rule among the `STREAM` receive surfaces it exposes
  publicly. A separate public release API such as `detachStream`,
  `streamDetach`, or a callback detach is not in core's public contract,
  so it is not added to the canonical binding surface. Releasing the
  receive mode and cleaning up the callback is handled by socket close.
- `zlink_recv_handler()` is exclusive to raw `STREAM`. Attaching it to
  `PAIR`/`DEALER`/`SUB`/`XSUB`/`ROUTER` fails with
  `ZLINK_HANDLER_NOT_SUPPORTED`.
- Socket defaults: `ZLINK_ROUTER_OPT_MANDATORY` = `1`,
  `ZLINK_OPT_RID_DUPLICATE_POLICY` = `ZLINK_RID_DUPLICATE_REJECT`, and
  `ZLINK_PUB_OPT_NODROP` = `0`.
  A binding's examples are written against these defaults.

## Actor Dispatch Binding Contract

This section is the Actor public contract that applies to every language
binding in common. A per-language document must spell out the contract
below using names and types that fit its own language convention.

Actor dispatch is not an add-on to SPOT messaging — it's an independent
public service-layer capability. Because `SpotNode`, `Spot`, and
`StreamSocket` share ownership of lifecycle and routing, each public
type exposes its own share of the responsibility. The exact surface
placement follows the `Actor Dispatch Policy` section below.

#### Actor Id, Ref, And Lifecycle Entry Points

- An Actor id is a non-empty UTF-8 string up to 255 bytes. A NUL
  character is not allowed.
- An Actor ref carries `node_rid`, `actor_id`, and `generation`.
  `generation == 0` is an unchecked remote ref, and is not treated as an
  invalid value.
- Creating an unchecked remote Actor ref is owned by `SpotNode`. It may
  be expressed as a static method or a factory function per language
  convention, but the canonical documentation and samples are based on
  the `SpotNode`-owned surface. Do not add a separate duplicate unchecked
  factory as public API directly on `ActorRef` itself.
- A local Actor is created by `SpotNode`. An Actor can join only one Spot
  at a time, and leaving does not drain unread messages.
- `Actor.close`, or an equivalent lifecycle method, destroys the local
  Actor owned by that Actor handle. `SpotNode.destroyActor(actorRef)`, or
  an equivalent method, is a ref-based destroy surface for a caller that
  holds only an Actor ref, without an Actor handle. These are not the
  same responsibility repeated under two names — they are two entry
  points with different owners, and a per-language spec must document
  this difference.
- `Actor.join` / `Actor.leave` are the surface for a caller holding a
  local Actor handle. `SpotNode.joinActor(actorRef, ...)` /
  `SpotNode.leaveActor(actorRef, ...)` are the surface for a caller
  holding only an Actor ref. Providing only one of the two makes either
  the ref-only flow or the owned-handle flow unnecessarily complicated.

#### STREAM Session Binding

- One STREAM session can bind multiple Actors. Bind/unbind is keyed on
  the session routing id and either the actor id or the Actor ref.
- A public API that sends from STREAM to an Actor uses the bound session
  and actor id as its selector. A removed lookup/send helper name is not
  kept in the public API.
- `Actor.sendBoundSession` and `Actor.closeBoundSession` do not take a
  session routing id as an argument. An Actor hides its current
  bound-session selection internally. When a caller must select
  explicitly by session routing id, it uses
  `StreamSocket.sendBoundActor(...)` instead.
- Actor recv info's `source_node_rid` and `source_session_rid` are value
  fields of the core struct, so they are not documented as
  nullable/optional. No-data is delivered only through the recv result's
  own representation, such as `false`, a no-data result, or `Ok(false)`.

#### Dispatch And Join Results

- An Actor readable dispatch event must let the caller know which Actor
  to drain. A language that hands the callback off to a different
  execution context must non-blockingly pre-drain the Actor part at
  callback-entry time, so the public dispatch info can return that part.
- A Spot join request carries a message. A join reply must also return a
  message to the caller together with the accept/reject result. Join
  completion must deliver the final Actor ref (for a remote join, the
  target node's ref) and the joined Spot rid to the application through
  a dedicated `actor join` result type.
- The request-reply surface exposes only the payload part the core reply
  function supports. Because the core reply function has no send-flag
  argument, a binding does not add a no-op flag-setting step to the reply
  builder.

#### Removed APIs

- Remote Actor creation and the admission handler have been removed from
  the public surface. An Actor that must start on a remote node is
  created by the application directly on that SpotNode with `actor_new`.
  When a checked ref for a remote Actor is needed, use the async
  `remote_actor_get_ref` lookup.
- Actor location is updated through the Actor creation, Spot join/leave,
  and Actor destroy flows. STREAM session bind/unbind neither creates nor
  removes an Actor location.
- Session attach and Actor location movement are different state
  transitions. Joining a user Spot does not require a bound STREAM
  session. Moving an Actor's location does not automatically change the
  session mapping.
- There is no per-Actor queue-limit option. A binding must not make this
  a public option.
- A removed Actor ref function, a stream actor lookup/send helper, or a
  session-actor-key design name is not kept in the public surface or
  documentation.


## Document Interpretation Rules
- This document's policy body is a normative document by default.
- The terms below carry the following meaning.
  - `Required`: an item that must be followed in the current review and
    implementation. Non-compliance blocks the review.
  - `Recommended`: an item strongly recommended, but which can be applied
    in stages depending on the binding's characteristics. Non-compliance
    requires a reason during review but does not block it.
  - `Target`: a goal item to align to over the long term. It applies only
    once a given binding decides to implement that component. If a
    binding decides not to implement it, review does not require it.
  - `Internal-only`: an item that can be used inside a binding's
    implementation, but must not be exposed through the public API,
    samples, guides, or spec signatures.
- Absent a separate marker, treat the policy body as `Required`.
- If a section title is marked `(Target)` or `(Recommended)`, that whole
  section is interpreted at the marked level. This takes priority over
  the unmarked default (`Required`).
- The `Implementation Review Checklist` section is not a design draft for
  adding a new API — it's the standard for confirming an implementation
  follows an already-defined public API contract.
- A checklist item does not replace the semantic contract defined in the
  document body.

## Core Principles
- The core contract's single baseline is `zlink.h`'s `*_part` substrate.
- The internal implementation of the send/recv/request/reply/publish/
  subscribe family must use the core `*_part` substrate. It does not call
  an aggregate-shaped core function directly from inside a binding.
- The public API is designed around a multipart model.
- Blocking and non-blocking can be distinguished by name.
- The same capability is not exposed redundantly through multiple paths.
- Value meaning is raised into an enum, boolean, or value object, not
  left as `int`.
- A raw option bag is not exposed publicly.
- A binding does not infer core's state errors.
- A binding blocks an input value's format, range, overflow, and
  truncation risk up front.
- Structure follows POSD principles, prioritizing deep modules,
  information hiding, and low change amplification.
- This document defines the semantic contract first.
- A per-language surface can differ to fit each language's convention,
  but the semantic contract must be the same.

## Monitor Ready Contract
- The `value` of a `*_READY_CHANGED` monitor event is not an aggregate
  ready-count contract.
- A binding's public API must not assume a monitor snapshot has a
  ready-count surface.
- When a readiness gate is needed, use the low-cost event edge directly.
- Raw perf/samples use `CONNECTION_READY` event counting.
- SPOT perf/samples do not use a separate service event gate.
- SPOT perf uses an explicit `READY`/`START` barrier protocol.
- Do not turn a delivery-ready/count-family monitor event into a new gate
  contract.

## POSD Structure Policy
- Binding design follows John Ousterhout's POSD principles.
- A public API must reduce the number of concepts a user needs to know.
- Internal implementation complexity must be hidden behind a facade,
  value object, or domain object.
- Avoid a shallow wrapper.
  - Do not multiply a public wrapper that only renames a native function
    without adding new meaning.
- Do not repeatedly expose the same capability through multiple types and
  multiple names.
- Gather a rule whose change must end in one place into a single module.
  - Example: a routing-id length limit
  - Example: the send-failure contract
  - Example: typed-option ownership
- A role, owner, no-data, error, or naming rule shared across languages
  is owned exactly once by this policy document. A per-language spec does
  not redesign the same rule — it expresses this document's contract in
  that language's convention.
- If a per-language spec needs a rule that differs from this document, do
  not change the individual document first. First write the exception
  reason and scope into this policy document, then update that language
  document. This keeps the same design decision from scattering across
  multiple documents.
- Reduce temporal decomposition.
  - Example: forbid an API where a user must remember the order in which
    to combine `setOption` calls
- A public API reveals "what it can do," not "how it's wired internally."
- Treat a value object and a result object as deep modules.
  - Give the caller a small interface, while encapsulating validation,
    ownership, and shape rules together internally.

## Public Surface Rules

### Base Type Exposure
- Where possible, let a user directly use only a concrete socket type at
  compile time.
- Avoid a structure where a user directly uses a generic root base, a raw
  compat base, or a shared base instead of a concrete socket type.
- A statically typed binding must enforce this rule using public
  type/export/visibility.
- A dynamic binding must enforce the same rule with export restrictions
  and surface tests.
- A generic root base or raw compat base exposes only the common
  lifecycle and common management functionality externally.
- A role-specific shared base may expose externally only the capability
  every descendant has in common.
- A socket-type-specific role must not be promoted to a generic root base
  or a raw compat base.
- Example common functionality a public base may allow external access
  to:
  - `bind`, `unbind`
  - `connect`, `disconnect`, `disconnectRid` on connectable base only
  - `close` / `dispose`
  - common typed options
  - `monitorOpen`, or an equivalent monitor entry point
  - `setTlsServer`, `setTlsClient`, or an equivalent TLS helper
- Functionality a generic root base or raw compat base must not allow
  external access to:
  - `send(...)`
  - `send(routingId, ...)`
  - `sendParts(...)`
  - `sendFrom(...)`
  - `recv()`
  - `recv(flags)` / `recv(size, flags)`
  - `recvInto(...)`
  - `recvMsgInto(...)`
  - a routed-receive alias (`receiveRouted`, and so on)
  - `publish(...)`
  - `setSubscription(...)`
  - `unsetSubscription(...)`
  - `subscribe()`
  - `receiveSubscriptionEvent()`
  - raw direct receive handler registration
  - `onSubscribe(...)`
  - `setSendReadyHandler(...)`
  - `setRoutingId(...)`, `getRoutingId()`
  - `attachStreamRaw(...)`, `detachStream()`
  - `streamAttach(...)`, `streamAttachRaw(...)`, `streamDetach()`
  - `streamPeerRoutingId(...)`, `streamSend(...)`
  - a raw option bag (`setOption`, `getOption`, `setSockOpt`,
    `getSockOpt`, and so on)
  - a topic/socket-type-specific option facade
  - a legacy alias that bypasses the canonical name
    - example: `recvHandler(...)`, `subscribeHandler(...)`
- A role-specific shared base can allow a role only when it's common to
  every descendant.
  - example: `setSubscription`, `unsetSubscription`, `subscribe` on a
    subscriber-only base
  - example: `publish`, `setSendReadyHandler` on a publisher-only base
- The roles above must exist as public only on a concrete socket type
  marked `Y` in the role matrix.
- A base-mediated bypass call must not be possible for a socket type
  marked `—` in the role matrix.
- Perf, sample, helper, and compat layers must not treat a base entry
  that bypasses the canonical public surface rule as a new baseline
  either.
- Even when a deprecated compat API is needed, isolate it into a compat
  namespace or internal surface separate from the canonical public API.
- A structure where a user must remember a `SocketType` and a raw flag
  combination to pick the right send/recv family is treated as a POSD
  violation.

### Multipart Only
- Unify the send/receive public surface around multipart.
- Do not put a single-message-receive convenience overload in the public
  surface.
- A single-part send convenience method can be allowed.
  - example: `send(Message part)` as a convenience overload of
    `send(List<Message> parts)`
- A receive result is returned as a language-appropriate domain object or
  an equivalent multipart representation.

### Error Handling Policy

Every data-path function (`send`, `recv`, `request`, `reply`,
`subscribe`, `publish`) follows the same error-handling principle.

#### Principles

1. **A language with exceptions does not deliver an error through a
   return value.**
   - Applies to: C++, Java, .NET, Node, Python.
   - Returns a result, or void, on success.
   - Throws an exception on failure.
   - The exception carries an `int code` (in the 0–706 range) so the
     caller can distinguish the failure cause.
   - Every failure including `BACKPRESSURED`, `NOT_CONNECTED`, and
     `NOT_FOUND` is delivered as an exception. These are never return
     values.
2. **C / Go / Rust have no exceptions, so they follow a return-based
   contract.** A binding handles it in the style each language's idiom
   fits.
   - C: returns a per-function typed result enum
     (`zlink_submit_result_t`, `zlink_recv_result_t`,
      `zlink_handler_result_t`, `zlink_close_result_t`,
      `zlink_bind_result_t`, `zlink_connect_result_t`,
      `zlink_config_result_t`).
   - Go: returns `(T, error)`. The error object carries an `int` code.
   - Rust: returns `Result<T, E>`. `E` is, where possible, a concrete
     per-function-family error (`BindError`, `SubmitError`, and so on),
     promoted to `ZlinkError` only at a boundary where multiple function
     families mix. An error value carries an `int` code. Callers use the
     `?` operator to propagate.
3. **Express blocking-vs-non-blocking through `flags` and the return
   rule, instead of a `Try*` name.**
   - C keeps the C ABI functional contract.
   - Go / Rust keep return-based error delivery, but still apply the
     wrapper binding's ref-out recv and operation builder rules.
   - `.NET` / `Java` / `Node` / `Python` / `C++` do not add a public
     `trySend`, `tryRecv`, or `tryRequest`.
   - The C ABI expresses blocking vs. non-blocking through a function
     argument `flags`.
   - A wrapper binding's send/publish/request/reply family expresses a
     non-blocking submit through the builder's `.flags(...)` step. It
     does not add a separate `flags` argument to the operation
     start-point signature.
   - A wrapper binding's data-plane `recv`, routed recv, and `subscribe`
     fill caller-provided result storage, and the return value expresses
     only "was data received."
   - When a non-blocking receive currently has no data, it returns a
     per-language no-data representation such as `false`, `nil, false`,
     or `Ok(false)`, and only a real error is delivered as an exception
     or a return error.
   - An async request is selected through the same `request` operation
     builder's completion-object-return step, and it does not take
     submit flags.
   - A transport-style name such as `sendNoWait`, `recvNoWait`, or
     `publishNoWait` does not belong on the public surface.
4. **Looking up `INTERNAL_ERROR` detail.**
   - When the result code is in the `INTERNAL_ERROR` family (12, 105,
     206, 306, 404, 505, 604, 704, and so on), the internal raw errno can
     be looked up with `zlink_errno()`.
   - The binding's error type (an exception object for exception
     languages, an error value for return-based languages) exposes this
     through an `internalErrno`/`internal_errno` field (for debugging
     only).
   - For every other result code, calling `zlink_errno()` is
     unnecessary.

#### Per-Language Error Representation

| Language | Handling | Error type | Code access | Internal errno |
|---|---|---|---|---|
| C | return | per-function result enum | the enum value itself | `zlink_errno()` |
| C++ | return / throw | caller-provided recv returns `int`; other failures use `zlink_error_t` | recv: the return value; exception: `.code()` | recv: `errno` when `-1`; exception: `.internal_errno()` |
| Java | throw | `ZlinkException` | `.getCode()` | `.getInternalErrno()` |
| .NET | throw | `ZlinkException` | `.Code` | `.InternalErrno` |
| Go | return | `error` | `.Code()` | `.InternalErrno()` |
| Rust | return (`Result`) | `ZlinkError` | `.code()` | `.internal_errno()` |
| Node | throw | `ZlinkError` | `.code` | `.internalErrno` |
| Python | throw | `ZlinkError` | `.code` | `.internal_errno` |

- The `return` group (C / Go / Rust) has the caller explicitly check the
  return value. Go uses `if err != nil`; Rust uses the `match`/`?`
  operator idiom.
- The `throw` group (C++ / Java / .NET / Node / Python) propagates an
  exception. The caller handles it with a per-language `try`/`catch` or
  by propagating further up.

#### Error Codes

- The C API returns a per-function typed result enum.
- Every enum value is unique across the 0–706 range.
- A binding includes this code in its per-language error type's `int
  code` (an exception object for exception languages, a return error
  value for return-based languages).
- For the full enum definition, see
  [errno-map.md](https://kairos-code-dev.github.io/zlink/en/spec/core/04-errno-map/).

#### Per-Function Error Type Hierarchy

**Every binding inherits the C API's per-function typed-result-enum
structure as-is.** With only a single `ZlinkException`/`ZlinkError`, a
caller could not know the set of possible errors from the signature
alone.

Each binding provides 8 function-family error types as subtypes of
`ZlinkException`/`ZlinkError`. A method signature must expose that
function family's concrete error type.

| C result enum | Function family | Subtype (semantic contract) |
|--------------|--------|--------------------------|
| `zlink_submit_result_t` | send / publish / request submit / reply submit | `SubmitError` |
| `zlink_request_result_t` | request completion (callback) | `RequestError` |
| `zlink_recv_result_t` | recv / subscribe / subscription event / monitor recv / timer recv | `RecvError` |
| `zlink_handler_result_t` | handler registration | `HandlerError` |
| `zlink_close_result_t` | close / destroy | `CloseError` |
| `zlink_bind_result_t` | bind | `BindError` |
| `zlink_connect_result_t` | connect / disconnect / unbind | `ConnectError` |
| `zlink_config_result_t` | option set/get, snapshot, poller mutation, proxy, timer config | `ConfigError` |

##### Per-Language Naming

| Language | Top-level type | Subtype naming | Base type | Example signature |
|------|-----------|----------------|----------|-------------|
| C | — | the per-function typed enum as-is | — | `zlink_bind_result_t zlink_bind(...)` |
| C++ | `zlink_error_t` | `zlink::<category>_error_t` (snake_case + `_t`) | the `std::runtime_error` family | `void bind(...) /* @throws bind_error_t */` |
| Java | `ZlinkException` | `<Category>Exception` | **unchecked** (`RuntimeException`) | `void bind(...) /* @throws BindException */` |
| .NET | `ZlinkException` | `Zlink<Category>Exception` | `System.Exception` (unchecked; every .NET exception is unchecked) | `void Bind(...) /* throws ZlinkBindException */` |
| Node | `ZlinkError` | `<Category>Error` | `Error` | `bind(ep): void /* @throws BindError */` |
| Python | `ZlinkError` | `<Category>Error` | `Exception` | `def bind(ep): ...  # raises BindError` |
| Go | `error` (interface) | `*<Category>Error` (typed error struct) | implements the `error` interface | `func (s) Bind(ep) error  // returns *BindError` |
| Rust | `ZlinkError` (enum) | `<Category>Error` (a variant or a separate type) | implements `std::error::Error` | `fn bind(ep) -> Result<(), BindError>` |

- `Category` is one of 8: `Submit`/`Request`/`Recv`/`Handler`/`Close`/
  `Bind`/`Connect`/`Config`.
- `ZlinkException`/`ZlinkError` stays the parent of every subtype,
  preserving the "catch-all" idiom. A caller catches the subtype when it
  needs granularity, or the parent otherwise.
- Each subtype error has its own dedicated `ErrorCode` nested enum for
  that function family. Another function family's codes are not
  expressed in that type.
- **Java / .NET follow the unchecked-exception system.** A method
  signature does not force a `throws` clause. A possible exception is
  documented with Javadoc `@throws` / XML doc
  `/// <exception cref="...">`.
- Rust / Go declare the concrete subtype error as the return type. A
  dynamic language (Node/Python) provides the same information with
  TSDoc `@throws` / a Python docstring `Raises:`.

##### Signature Declaration Rules

- When a method can throw/return only a single function family's error,
  declare only that concrete subtype.
  - Java: `@throws BindException` (Javadoc; no `throws` clause needed in
    the signature)
  - .NET: `/// <exception cref="ZlinkBindException">`
  - C++: `/// @throws bind_error_t` (do not mark it `noexcept`)
  - Node: TSDoc `@throws {BindError}`
  - Python: docstring `Raises: BindError`
  - Go: document the return type as `returns *BindError`
  - Rust: return type `Result<T, BindError>`
- When a method spans multiple function families (for example, a service
  layer's combined call), declare the shared parent
  `ZlinkException`/`ZlinkError` and list the subtypes that can actually
  occur in the doc.
- A validation exception (a language-native `IllegalArgumentException`,
  and so on) is separate from the system above and does not enter the
  `ZlinkException`/`ZlinkError` hierarchy.

### Flags Policy

Every data-path function has a `flags` option. An ordinary socket
function expresses it as a per-language signature's `flags` parameter,
and a function targeting a SPOT operation builder expresses it as the
builder's `flags(...)` step.

| Function family | `flags` usage |
|---|---|
| `send`, `publish`, `reply` | `DONTWAIT` — non-blocking submit |
| `recv`, `subscribe`, `receiveSubscriptionEvent` | `DONTWAIT` — non-blocking receive |
| `request` (callback) | `DONTWAIT` — non-blocking submit |
| `request` (async completion) | No flags — uses the per-language completion-object-return path |

- The default `flags` value is `0` (blocking).
- A non-blocking call's temporary state is delivered per each language's
  public contract.
  - `.NET` / `Java` / `Node` / `Python`
    - `send`, `publish`, callback `request`: `false` on temporary
      backpressure
    - caller-provided `recv`, `subscribe`,
      `receiveSubscriptionEvent`: `false` when there is currently no
      data
    - Any other failure: a typed exception
  - C++
    - operation builder `send` / `publish` / callback `request`: `false`
      on temporary backpressure
    - caller-provided `recv` / `subscribe` /
      `receive_subscription_event`: returns the `recv_result_t::no_data`
      integer value when there is currently no data
    - only a binding-local failure returns `-1` and sets `errno`
  - Return-based languages (C/Go/Rust): return the error (C = result
    enum, Go = `error`, Rust = `Err(E)`).
- Per-language `flags` representation:
  - C: `int flags = 0` (the C ABI does not apply the builder policy)
  - C++ / Java / .NET / Node / Python / Go / Rust send/request/reply/
    publish/Actor-attach surfaces: expressed through the builder's
    `.flags(...)` step. No separate `flags` argument or `_with_flags`
    variant is added to the operation start-point signature.
  - C++ / Java / .NET / Node / Python / Go / Rust data-plane
    recv/subscribe surfaces: take a `flags` argument together with
    caller-provided output storage.

### Naming Policy

#### Creation Function Naming

A public function that builds a new object from an input value, such as
`Message` and `RoutingId`, aligns to the `.NET` binding's `From(...)`
meaning. Avoid putting the input type into the function name — doing so
splits the same concept into multiple names across languages.

- Gather ordinary construction under a single `from(...)`, or that
  language's equivalent name. A type-suffixed name such as `from_bytes`,
  `from_string`, `from_u32`, or `from_uuid` is not used as canonical
  public API.
- Hex decoding is allowed as an exception, under the `from_hex` family,
  because it carries a distinct meaning — decoding a human-readable
  string. The per-language spelling follows idiom: `FromHex`, `fromHex`,
  `from_hex`, `NewRoutingIDFromHex`.
- Python uses `from_(...)` because `from` is a reserved word.
- Rust uses the standard `From` implementation for a routing id, and can
  use the `try_from` idiom for a message construction that can fail. It
  does not add an input-type-named public helper such as `from_bytes`/
  `from_string`.
- Because Go has no overloads, it allows a typed constructor such as
  `NewRoutingID(...)`, `NewRoutingIDString(...)`,
  `NewRoutingIDUint32(...)`, and `NewRoutingIDUUIDBytes(...)`. This
  exception exists to preserve Go's static-typing style, and it does not
  repeat both `From` and the type name together, as in
  `NewRoutingIDFromString`.
- Allocation is not a source conversion, so it uses `allocate(...)` or a
  per-language constructor idiom (`NewMessageWithSize(...)`, and so on).

#### One Entrypoint, Variation Expressed As Builder Steps

Variations of the same operation — async/callback, single/multipart,
with or without flags, with or without a timeout — use the same
entrypoint name, and the variation is expressed as a builder step. Do not
create a separate name such as `request_callback`, `send_nonblocking`, or
`send_with_flags`.

```
// GOOD: one name, builder absorbs the form.
spot.request_to_channel(channel)
    .message(part)
    .timeout(Duration::from_secs(3))
    .submit()                              // returns the language completion object

spot.request_to_channel(channel)
    .message(part)
    .flags(SendFlags::DONTWAIT)
    .submit(callback)                      // callback variant

// BAD: split names for the same operation.
request_to_channel(channel, parts, timeout)
request_to_channel_callback(channel, parts, callback, flags, timeout)
```

#### Shared Result Type Names

A shared result type does not repeat its owner's name. The type name
reveals the domain concept the value represents, directly.

- The canonical name for a poller wait-result type is `PollEvent`. C++
  uses `poll_event_t`; Java/.NET use `PollEvent`; Node/TypeScript use
  `PollEvent`. A name that appends the owner a second time, like
  `PollerEvent`, is not used as canonical public API.
- Timer, monitor, and dispatch results follow the same rule. When the
  owner is already clear from the return type or namespace, the type
  name does not repeat it.

#### SPOT Target Naming

SPOT routed naming separates pub/sub from targeted messaging.

- **Channel-aware path**
  - `send_to_channel(channel_name) -> SendOp`
  - `request_to_channel(channel_name) -> RequestOp`
- **SPOT topic path**
  - `publish(topic) -> SendOp`
    - The receiver is already a publish-capable socket or `Spot`, so it
      does not repeat owner or parameter meaning, as in `publish_spot`
      or `publish_to_topic`.
- **Direct routed path**
  - `send_to_spot(dest_node_rid, dest_spot_rid) -> SendOp`
  - `request_to_spot(dest_node_rid, dest_spot_rid) -> RequestOp`
  - `request_to_router(peer_rid) -> RequestOp`
- **Reply path**
  - `reply_to_spot(dest_node_rid, dest_spot_rid, request_seq) -> ReplyOp`
  - `reply_to_router(peer_rid, request_seq) -> ReplyOp`

The payload and options of `SendOp`, `RequestOp`, and `ReplyOp` are
expressed through the `message(...)`, `flags(...)`, `timeout(...)`, and
`submit...` steps the `Operation Builder Policy` section defines. So a
new canonical SPOT surface does not add a `Message`/`List<Message>`/
`flags`/`timeout` combination overload on the same start point.

On a new SPOT binding surface, `send_to_channel`/`request_to_channel`/
`publish(...)` are treated as the default path, instead of the old
`send_service`/`request_service`. A direct address-targeted path can be
separately supported as core's typed routed surface.

Convert to camelCase / PascalCase / snake_case per each language's
convention.

### Request Policy

A request can offer both a per-language async-completion form and a
callback-completion form, and both are selected at the submit step of
the same `RequestOp` operation builder the `request` entrypoint returns.
Do not create a separate name (`request_callback`, `requestAsync`, and
so on).

For a SPOT operation builder target, the work start point is
`requestToChannel`/`requestToSpot`/`requestToRouter`; for a raw
`DealerSocket`/`RouterSocket`, the work start point is
`request`/`request(peer)`. Regardless of the start point, the completion
mode is selected through the per-language final execution method the
[bindings async execution surface policy](async-coroutine-policy.md)
defines.
- **On success, it returns only the reply payload's `List<Message>`.**
  The caller already knows the `routing_id` and `request_seq` of the
  request it sent, so it does not need `Received` back. A separate
  `Reply` type is not created.
- Because multipart reply is possible, it returns `List<Message>`, not a
  single `Message`. A single-part reply is retrieved with `list[0]`.

#### Callback Request

The builder's callback submit method (`submit(callback)`).

- Takes a flags parameter. Delivered through the builder's `.flags(...)`
  step; a non-blocking submit is possible with `DONTWAIT`.
- The timeout is delivered through the builder's `.timeout(...)` step. If
  not specified, it uses the socket's default timeout.
- The submit step is interpreted as follows.
  - Exception-based languages: blocking success = `true`, non-blocking
    temporary backpressure = `false`, any other submit failure = an
    exception
  - Return-based languages: keeps the existing error-return contract
  On failure, the callback is not registered.
- On submit success, the callback is called exactly once.
  - Success: `result = OK`, includes reply parts
  - Failure: `result != OK` (`TIMED_OUT`, and so on), parts is
    empty/null/None/`Option::None`
- The callback signature follows language idiom, and **delivers the
  reply payload as `List<Message>`** (not `Received`):
  - The common pattern (C++/Java/.NET/Node/Python/Go):
    `(RequestResult result, List<Message> parts)` — a result enum and a
    parts list
  - Rust idiom: `FnOnce(Result<Vec<Message>, RequestError>)` — this
    pattern is allowed because `Result` is Rust's standard way to
    express an error plus a value. `RequestError::code` maps 1:1 to the
    `RequestResult` enum value.

#### Shared

- For the full `zlink_request_result_t` definition, see
  [errno-map.md](https://kairos-code-dev.github.io/zlink/en/spec/core/04-errno-map/).
- Because Go / Rust have no exceptions, a callback request's submit
  failure is also handled in a return-based way (Go: returns
  `*SubmitError`; Rust: returns `Result<_, SubmitError>`).

## Domain Object Policy
- Java, C#, Go, Rust, Node, and Python prefer a domain object over an
  `out` parameter or a raw tuple where possible.
- The minimum core domain model:
  - `Message`
  - `RoutingId`
  - `Received`
  - `TopicMessage`
  - `SubscriptionEvent`
  - `SubmitResult` (C / Go / Rust — included in the return object/error
    for return-based languages; exposed as the exception object's
    `.code` for exception languages)
- A result object must describe payload shape, ownership, and optional
  routing metadata together.
- A convenience feature is a method on the result object.
  - example: `singlePartOrThrow()`

### Domain Object Canonical Shape (Shared By Every Binding)

Every domain object exposes the canonical field/method set below **as
is**. Only the naming convention (camelCase / snake_case / PascalCase)
is converted per language — **the field type and method meaning do not
change.** A per-language idiomatic convenience method can be added, but
it must not replace or partially omit a canonical method.

#### `Message`

A single message part that carries a transport payload. Every send/
request/reply/publish builder accumulates one or more `Message`s to
build a multipart payload.

| Member | Type | Meaning |
|------|------|------|
| empty constructor | ctor/static | Creates a zero-length message |
| `allocate(size)` | static/ctor | Creates a `size`-byte payload buffer |
| `from(bytes)` | static/ctor | Copies bytes-like input into message-owned storage |
| `from(string)` | static/ctor | Encodes a user string as a UTF-8 payload |
| `copy()` / `from(Message)` | `Message` | Copies the source payload into a new message |
| `move()` / consume path | `Message` / builder step | An explicit ownership transfer; the source cannot be reused after the call |
| `size` | `int` / `usize` | Payload byte length |
| `is_empty()` | `bool` | `size == 0` |
| `to_bytes()` | `bytes` / `byte[]` / `Vec<u8>` | A snapshot copy of the payload |
| `data` / `as_bytes()` | view | A read view of the payload; no lifetime guarantee after close |
| `mutable_data` / `as_mut_bytes()` | mutable view | A mutable view for filling an allocated payload |
| `copy_to(destination)` | `int` / `bool` | Copies the payload into a caller-provided buffer |
| `to_string()` / `as_str()` | `string` / result | A UTF-8 decode convenience |
| `get_property(name)` | `string?` / result | Looks up a native message string property |
| `ref_count()` | `int` | A native-storage reference-count diagnostic value |
| `close()` / `Dispose()` / `Drop` | — | Cleans up native storage, per each language's lifecycle idiom |

The name follows per-language idiom. The meaning fits the slots below.

| Meaning | .NET | Java | Node | Python | Rust | C++ | Go |
|------|------|------|------|--------|------|-----|----|
| Empty message | `new Message()` | `new Message()` | `Message.from(Buffer.alloc(0))` or equivalent | `Message()` | `Message::new()` | `message_t()` | `NewMessage(nil)` |
| Sized allocation | `Allocate(size)` | `allocate(size)` | `allocate(size)` | `allocate(size)` | `with_size(size)` / `allocate(size)` | `allocate(size)` | `NewMessageWithSize(size)` |
| Bytes copy | `From(bytes)` | `from(byte[])` | `from(BufferLike)` | `from_(buffer)` | `try_from(bytes)` | `from(...)` | `NewMessage(data)` |
| UTF-8 string | `From(string)` | `from(String)` | `from(string)` | `from_(str)` | `TryFrom<&str>` or equivalent | `from(std::string)` | `NewMessageString` |
| External buffer copy | — | `from(ByteBuffer)` / `from(ByteBuf)` | `from(BufferLike)` | `from_(buffer)` | `try_from(bytes)` | `from(...)` | `NewMessage(data)` |
| Message copy | `Copy()` | `from(Message)` | `copy()` or `from(Message)` | `copy()` | `Clone` or `try_clone()` | copy constructor | `Clone()` / `Copy()` |
| Explicit move | `Move()` / `MoveMessage(...)` | `move()` / `moveMessage(...)` | `moveMessage(...)` | `move_message(...)` | move-by-value | move constructor / rvalue builder | `MoveMessage(...)` |
| Bytes snapshot | `ToArray()` | `toByteArray()` | `toBytes()` | `to_bytes()` | `to_vec()` | `to_bytes()` | `BytesCopy()` or equivalent |
| Read view | `AsReadOnlySpan()` | `dataBuffer()` | `data()` | `data` | `as_bytes()` | `bytes()` | `Data()` |
| Mutable view | `AsSpan()` | `mutableDataBuffer()` | `data()` | `data` | `data_mut()` | `bytes()` / `data()` | `Data()` |
| UTF-8 decode | `GetString()` | `toUtf8String()` | `toString()` / `getString()` | `to_string()` / `decode` helper | `as_str()` | `to_string()` | `String()` / `Text()` |
| Property | `GetProperty(name)` | `getProperty(name)` | `getProperty(name)` | `get_property(name)` | `get_property(name)` | `property(name)` | `GetProperty(name)` |
| Refcount | `RefCount` | `refCount()` | `refCount()` | `ref_count()` | `ref_count()` | `ref_count()` | `RefCount()` |

Rules:
- The `from(bytes)` family always copies into message-owned storage. The
  caller must be free to change or release the input buffer afterward.
- Java's `from(ByteBuf)` copies a Netty `ByteBuf`'s readable bytes
  without changing `readerIndex`. `copyTo(ByteBuf)` writes into the
  destination's writable region and advances `writerIndex`.
- A borrowed/zero-copy constructor is not part of the canonical public
  contract. Even if a particular binding uses one as an internal
  optimization, the public API must not push lifetime responsibility
  onto the caller.
- The `message(...)` builder step follows the original-preservation
  contract. The caller must be able to reuse the message it passed even
  after a submit failure.
- Ownership transfer is allowed only through a separate path whose name
  reveals consume semantics — `move`, `MoveMessage`, move-by-value. This
  path must document that the original message cannot be reused even
  after a submit failure.
- `to_bytes()` is a snapshot copy. Allocation-free payload access is kept
  separate as a read-view API (`data`, `as_bytes`, `AsReadOnlySpan`, and
  so on).
- A read/mutable view is valid only until the message is closed,
  disposed, or dropped. A binding does not guarantee view usage after
  close.
- `get_property(name)` is a diagnostic/interop API for reading native
  message metadata. A property-write API is not part of the shared
  required contract.
- `ref_count()` is a diagnostic value. Do not build a public contract
  that judges ownership policy or send eligibility from the reference
  count.
- An RAII language (C++, Rust) does not have to explicitly expose
  `close()`. An explicit-lifecycle language (.NET, Java, Python, Go)
  must provide an idempotent close/dispose.
- The behavior of `size`, `data`, and `get_property` on a closed or
  moved-from message follows per-language convention, but must document
  whether it returns an empty value or throws an exception/error.

#### `TopicMessage`

The recv result for raw `SUB`/`XSUB` and `Spot subscribe`. Raw pub/sub
wraps C API `zlink_subscribe_part()`, and Spot subscribe wraps
`zlink_spot_subscribe_part()`, into a single binding domain object. The
binding's public API assembles the part-helper call result into a
per-language multipart object and returns it.

| Member | Type | Meaning |
|------|------|------|
| `routing_id` | `RoutingId?` (optional) | The sender's routing id; null/None/empty if the transport doesn't carry one |
| `topic` | **`string` (UTF-8)** | The matched topic. **Not bytes.** |
| `parts` | `List<Message>` / `Vec<Message>` | The multipart payload |
| `is_single_part()` | `bool` | `parts.size() == 1` |
| `first_part()` | `Message` | `parts[0]`; error/exception if empty |
| `single_part_or_throw()` | `Message` | Returns the part if `is_single_part()`, else error/exception |
| `close()` / `Dispose()` / `Drop` | — | Cleans up held parts, per each language's lifecycle idiom |

Rules:
- Do not create `Subscribed` or a similar subclass. Expose only
  `TopicMessage`.
- A Spot subscribe result exposes `topic + parts` together. The channel
  is state that already lives on the `SpotNode` the `Spot` handle is
  bound to, so it is not repeated as a message result field.
- `topic` is a UTF-8 `string`. It is not exposed as `bytes`/`byte[]`/
  `Vec<u8>` (even if it arrives internally as raw bytes, the public API
  decodes it).
- Keep only a single typed `RoutingId` field. Do not create a dual
  property such as `RoutingId: string` plus `RoutingIdValue: RoutingId?`.

#### `Received`

The single canonical domain object that carries a PAIR / DEALER / ROUTER
/ STREAM / SPOT routed recv result. Other than lacking a topic field, it
has the same convenience-method set as `TopicMessage`. A routed recv
result provides a `send()` operation builder for sending an ordinary
response, and a request-reply result also provides a `reply()` builder.
Both entrypoints accumulate payload and options through builder steps,
per the `Operation Builder Policy`.

`Received` is not a per-socket-kind message wrapper. Request meaning is
the same across DEALER, ROUTER, and SPOT, and is expressed only through
`request_seq` and reply context. A binding must not add a
protocol-specific public result type such as `DealerReceived`/
`RouterReceived`/`SpotReceived` as a new canonical surface. If an
existing binding has such a type, remove it, and new code, samples,
perf, and framework integrations must use `Received`.

| Member | Type | Meaning |
|------|------|------|
| `routing_id` | `RoutingId?` | The sender's routing id (router = `peer_rid`, spot = `source_node_rid`) |
| `spot_rid` | `RoutingId?` | Set only for SPOT routed recv (`source_spot_rid`) |
| `request_seq` | `uint64?` | Set in request-reply mode; otherwise null |
| `parts` | `List<Message>` | The multipart payload |
| `is_single_part()` | `bool` | Same as above |
| `first_part()` | `Message` | Same as above |
| `single_part_or_throw()` | `Message` | Same as above |
| `send()` | `SendOp` | An operation builder that sends an ordinary routed message back to this `Received`'s sender; `SubmitError` at submit time if there's no routed source context |
| `reply()` | `ReplyOp` | A reply operation builder valid only when this was a request; `SubmitError` at submit time if `request_seq` is absent or the reply context is invalid |
| `close()` / equivalent | — | Same as above |

In .NET, `Received.Create()` is the canonical construction path for
caller-provided recv storage. `Received` stays a public concrete
contract type.

`request_seq` rules:
- `null`/`None`/an empty `Optional`/`hasRequestSeq == false` means an
  ordinary receive result.
- `0` is not exposed as "has a request" on the public high-level
  `Received`. A high-level binding converts a core out-param's
  `request_seq == 0` into absent.
- A non-zero `request_seq` means a receive result that has request-reply
  context. This meaning is the same across DEALER / ROUTER / SPOT.
- A substrate-level distinction such as a request/reply message type must
  not split the public `Received` meaning. If such a value is genuinely
  needed as a public contract, expose it only as `Received`'s shared
  metadata, not as a protocol-specific result type.

`send()` rules:
- Independent of whether it was a request. It can be called as long as
  routed source context exists, even without `request_seq`.
- `send()` has no request-reply meaning. It simply sends an ordinary
  routed message back toward whoever sent this `Received`.
- A `ROUTER` and `STREAM` receive result sends by peer routing id. A
  `SPOT` routed receive result sends by source node rid and source spot
  rid.
- Payload accumulation and options such as `flags(...)` are expressed
  through `SendOp` builder steps, and a non-blocking submit flag such as
  `DONTWAIT` is also delivered through the builder's `.flags(...)` step.

`reply()` rules:
- **Calling it is forbidden when `request_seq` is `null`.** Calling it
  anyway is handled as a `SubmitError`-family failure at the builder's
  submit step. An invalid reply context — `request_seq == 0`, an invalid
  `(routing_id, request_seq)` combination, and so on — is treated as the
  same submit domain.
- `Received` internally holds a reference to the source socket (injected
  by the binding when it builds `Received` inside recv/handler).
- Calling `reply().submit()` after the socket has closed returns
  `SubmitError(TERMINATED)`.
- The server-side user does not need to separately store
  `(peerRid, requestSeq)` — `Received` alone is self-contained.
- A separate `router.reply(peerRid, seq).message(...).submit()` path is
  also kept for pull-mode compatibility, but **the recommended path is
  `received.reply().message(...).submit()`**.

#### `SubscriptionEvent`

The subscribe/unsubscribe event XPub receives, and the recv result for a
Spot subscription event.

| Member | Type | Meaning |
|------|------|------|
| `routing_id` | `RoutingId?` | The subscriber's routing id |
| `topic` | `string` (UTF-8) | The subscribed/unsubscribed topic |
| `subscribed` | `bool` | true = subscribe, false = unsubscribe |

Rules:
- Expose it only as a value object (no methods, fields only).
- No lifecycle such as `close()` (it's a value type).
- A Spot subscription event result exposes `topic + subscribed`.

#### `RoutingId`

A routing-id value object. Binary-safe (1–255 bytes).

| Member | Type | Meaning |
|------|------|------|
| `bytes` / `data` | `bytes` / `byte[]` / `Vec<u8>` / `Buffer` | The raw bytes (an immutable view) |
| `size` | `int` (1–255) | The byte length |
| `from(bytes)` | static/ctor | Builds from raw bytes |
| `from(value: string)` | static/ctor | Encodes a user string as UTF-8 bytes |
| `from_hex(value)` | static/ctor | Rebuilds from a hex string produced by `to_hex()` |
| `from(value: uint32)` | static/ctor | Builds a 4-byte big-endian `uint32` routing id |
| `from(value: guid)` | static/ctor | Builds a 16-byte UUID routing id |
| `to_bytes()` | `bytes` | Returns the original bytes |
| `to_hex()` | `string` | Displays the raw bytes as a hex string |
| equality / hash | — | Per-language idiom (`equals`/`hashCode`, `__eq__`/`__hash__`, `PartialEq+Eq+Hash`) |

The name follows per-language idiom. The meaning fits the slots below.

| Meaning | .NET | Java | Node | Python | Rust | C++ | Go |
|------|------|------|------|--------|------|-----|----|
| User string | `From(string)` | `from(String)` | `from(string)` | `from_(str)` | `From<&str>` | `from(std::string)` | `NewRoutingIDString` |
| Raw bytes | `From(bytes)` | `from(byte[])` | `from(Buffer)` | `from_(bytes)` | `From<&[u8]>` | `from(bytes)` | `NewRoutingID` |
| Hex round-trip | `FromHex` | `fromHex` | `fromHex` | `from_hex` | `from_hex` / `try_from_hex` | `from_hex` | `NewRoutingIDFromHex` |
| uint32 | `From(uint)` | `from(long)` | `from(number)` | `from_(int)` | `From<u32>` | `from(uint32_t)` | `NewRoutingIDUint32` |
| UUID | `From(Guid)` | `from(UUID)` | 16-byte `from(Buffer)` | `from_(uuid.UUID)` | `From<[u8; 16]>` | `from(std::array<uint8_t, 16>)` | `NewRoutingIDUUIDBytes` |

Rules:
- **A binary-safe value type.** Because a user-set routing id is usually
  a human-readable string, the string overload `from(value)` means UTF-8
  encoding. Arbitrary bytes received from native/core are preserved
  through the bytes overload `from(bytes)`.
- A `from_hex(value)` input allows only hex characters. A hex string is
  at most 510 characters, and it must fail with a per-language exception
  or error code if the decoded routing id exceeds 255 bytes.
- A value core treats as `uint32_t`, such as a 4-byte STREAM routing id,
  is handled through a typed API such as `from(value: uint32)`/
  `try_to_uint32(out value)`.
- A 16-byte UUID value is handled through a typed API such as
  `from(value: guid)`/`try_to_guid(out value)`.
- `to_string()`/`String()` is a per-language display string. It's
  recommended to show printable UTF-8 as-is, a 4-byte `uint32` as a
  numeric string, a 16-byte UUID as a UUID string, and anything else as a
  hex display prefixed with `hex:`. Use `to_hex()`/`from_hex(value)` for
  round-trip storage.
- Immutable. Once created, its content cannot change.
- Caching is not an observable contract. A binding can internally use a
  hash, a native struct, or a short-lived cache on the recv hot path if
  needed, but equality must always be judged by the bytes value, and a
  cache hit must not change API behavior.
- On Node, expose the `RoutingId` wrapper type as-is instead of a raw
  `Buffer`.

#### `MonitorEvent`

An event a socket monitor emits. **Required for every binding to
expose.**

| Member | Type | Meaning |
|------|------|------|
| `event` | `MonitorEventType` (enum) | The event kind (CONNECTION_READY, CONNECTED, DISCONNECTED, and so on) |
| `value` | `uint32` | A per-event detail value (for example, the reason code on DISCONNECTED) |
| `routing_id` | `RoutingId?` | The matching peer routing id (null for an event without one) |
| `local_addr` | `string` | The local endpoint |
| `remote_addr` | `string` | The remote endpoint |

#### `MonitorStatus`

The runtime status snapshot a socket monitor provides. **Required for
every binding to expose.**

| Member | Type | Meaning |
|------|------|------|
| `source_kind` | enum | The kind of monitored target |
| `state_flags` | enum flags | The state bitmask |
| `detail_flags` | enum flags | The detail bitmask |
| `snd_pending_msgs` | `uint64` | The number of messages pending in the send queue |
| `rcv_pending_msgs` | `uint64` | The number of messages pending in the receive queue |
| `auto_hwm_*` diagnostic fields | enum / number / bigint | Must expose the canonical auto-HWM fields of C's `zlink_monitor_status_t` with the same meaning. Includes enabled, profile (enum), role, policy class, unit budget, size cap, socket message slots, effective message bytes, applied HWM, applied buffer, the recent recalculation reason (enum), deferred shrink, and blocked ratio |
| `is_ready()` | `bool` | A convenience method that checks the ready bit in `state_flags`, for a raw socket monitor source only |

#### Service-Layer Entry Objects

The following are value objects returned from service-layer snapshot/
query calls. Every binding must **spell out the field list in its
spec** (a raw C struct must not be exposed as-is — wrap it in
per-language named fields).

- `SpotNodeStatus` — a spot node status snapshot
- `SpotNodePeerEntry` — a spot node peer entry. Must include `weight`.
- `SpotNodeSubjectEntry` — a spot node subject entry

Each spec spells out these types' fields as a table or code block. `C++`
wraps the raw `zlink_*_t` struct as `class <name>_t { ... }` rather than
exposing it directly on the binding API surface.

An extra method/field beyond the canonical set above is a policy
violation. When a per-language spec is found missing one, fill it in
against the canonical baseline, and remove any added non-standard
method.

## Socket Type Capability Policy
- Expose a per-socket-type capability only on that type itself.
- An unrelated socket must not have access to an unrelated function.
  - example: no publish/subscribe/xpub control surface on `PairSocket`
  - example: no general connect surface on `StreamSocket`
- Also expose a per-socket-type option only through that type's own role
  facade.

### Socket Class Naming/Structure Rule (Important)
- **A socket class name follows the core C API's socket-type name
  as-is**: `PairSocket`, `PubSocket`, `SubSocket`, `XPubSocket`,
  `XSubSocket`, `DealerSocket`, `RouterSocket`, `StreamSocket`. A binding
  must not rename it arbitrarily or add a synonym (`ClientSocket`,
  `BrokerSocket`, and so on).
- **A socket's capability functions (`send`, `recv`, `request`, `reply`,
  `publish`, `subscribe`, `on*` handlers, and so on) are exposed directly
  as methods on the socket class.** Do not create a separate wrapper/
  "helper" class for a single function or a narrow role such as
  request-reply (`RequestDealer`, `RequestRouter`, `DealerClient`,
  `RouterRequester`, and so on).
  - Reason 1: the C API's contract places
    `zlink_dealer_request_part()`/`zlink_router_request_part()`/
    `zlink_router_reply_part()` directly on the raw socket handle. The
    binding surface must preserve this structure to keep the core ↔
    binding mapping 1:1.
  - Reason 2: a wrapper class creates a duplicate lifecycle — "having to
    carry around another wrapped socket."
  - Reason 3: the role is easy to misread as inverted from the name
    (`RequestDealer` can be misread as "dealing requests").
- Keep implementation state such as a future/promise completion
  linkage (a pending map, and so on) inside the socket class, and expose
  only methods externally.
- The only exception is a service-layer surface that **combines
  different socket types** — these are independent service contracts,
  not a single-socket-function wrapper.
- This rule applies identically across every binding
  (C++/Java/.NET/Node/Python/Go/Rust), and a violation found in a spec
  file is **fixed immediately**.

### Socket Capability Matrix
- This table defines the capability each socket type must have, based on
  the `core/include/zlink.h` C API.
- This table is a public capability contract shared across every
  language binding. Functionality must not differ between bindings.
- A per-language difference is allowed only in how the same capability is
  expressed for that language's convention — casing, overloads,
  nullable, exception/error representation.
- Every binding treats this table as the answer key when writing surface
  tests.
- `Y` means every binding must expose that capability as public API.
- `—` means no binding may expose that capability as public API.

#### Connection Capabilities

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `bind` | Y | Y | Y | Y | Y | Y | Y | Y |
| `unbind` | Y | Y | Y | Y | Y | Y | Y | Y |
| `connect` | Y | Y | Y | Y | Y | Y | Y | — |
| `disconnect` | Y | Y | Y | Y | Y | Y | Y | — |
| `disconnectRid` | Y | Y | Y | Y | Y | Y | Y | — |

`disconnectRid` is the peer-rid disconnect surface of a connectable raw
socket. `STREAM` is a bind-only socket and does not expose `connect`,
`disconnect`, or `disconnectRid` as public API. `Spot` also does not
expose a raw peer-rid disconnect — disconnecting a SPOT node peer is
handled by the `SpotNode.disconnectPeerRid` family.

#### Send Capabilities

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `send` | Y | Y | — | — | — | — | — | — |
| `send(routingId)` | — | — | Y | — | — | — | — | Y |
| `publish` | — | — | — | Y | — | Y | — | — |

#### Receive Capabilities

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `recv` | Y | Y | Y | — | — | — | — | Y |
| `subscribe` | — | — | — | — | Y | — | Y | — |
| `receiveSubscriptionEvent` | — | — | — | — | — | Y | — | — |

#### Subscription Management

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `setSubscription` | — | — | — | — | Y | — | Y | — |
| `unsetSubscription` | — | — | — | — | Y | — | Y | — |

#### Callback Capabilities

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `setPacketHandler` | — | — | — | — | — | — | — | Y |
| `onReceive` | — | — | — | — | — | — | — | — |
| `onSubscribe` | — | — | — | — | — | — | — | — |
| `setSendReadyHandler` | Y | Y | Y | Y | — | Y | — | Y |

The `STREAM` public surface must provide `recv` and `setPacketHandler`.
The raw direct callback `onReceive` is not canonical public binding API.
Attempting to attach a different receive mode while one is already
active returns `HandlerResult::BUSY` (or the equivalent `EBUSY`). A
public `detachStream`/`streamDetach`-family release API is not provided.

#### Typed Option Capabilities

| Option Facade | Applies to |
|---|---|
| Common options (linger, HWM, timeout, and so on) | All |
| Router options (mandatory, handover, probe, connectRoutingId) | Router |
| Dealer options (probe) | Dealer |
| Stream options (notify) | Stream |
| Pub options (verbose, verboser, noDrop, manual, and so on) | Pub, XPub |
| Sub options (topicsCount) | Sub, XSub |
| RoutingId (set/get) | Dealer, Router, Stream |

  `disconnectRid`, `unbind`, and `close` are blocked.

## Per-Language Spec File Compliance Rule

Each per-language spec file (`doc/spec/bindings/{lang}/README.md`) must
follow the rules below. Apply this checklist when writing or reviewing a
spec file.

### Capability Matrix Consistency
- A per-language spec must not omit or add to the functionality in the
  Socket Capability Matrix above.
- Each socket-type class must provide every capability marked `Y` in the
  Socket Capability Matrix above, as a public surface that fits that
  language's convention.
- A capability marked `—` must not exist on that socket-type class in any
  language binding.
- Service-layer functionality the Socket Capability Matrix doesn't cover
  may be exposed as public API only when it's specified in a separate
  role matrix or policy section.
- Watch especially for these frequent violations:
  - No plain `send` (a send without routingId) on `RouterSocket`/
    `StreamSocket` — it must be `send(routingId, ...)`.
  - No `connect`, `disconnect`, or `disconnectRid` on `StreamSocket` —
    `STREAM` is a bind-only socket. Allowed only on Dealer, Router, Pub,
    Sub.
  - No `onSubscribe` callback on `XPubSocket` — only
    `receiveSubscriptionEvent` is allowed on XPub.
  - No `STREAM` raw direct callback `onReceive`, and no
    `detachStream`-family release API — the canonical surface is the
    `recv`/`setPacketHandler`/`close` combination.
  - Do not omit the shared socket TLS helper (`setTlsServer`,
    `setTlsClient`, or an equivalent name) — TLS configuration is
    shared transport functionality, so it must sit at the same location
    on every raw socket type.

### Required Argument For Routed Send
- `RouterSocket`'s and `StreamSocket`'s send must take routingId as a
  **required** argument.
- Making routingId an optional/default parameter is forbidden, because it
  would allow a plain send.

### Send/Publish Return Value
- On `.NET`/`Java`/`Node`/`Python`/`C++`, a blocking `send`/`publish`/
  callback `request` submit always returns `true` on success.
- On a non-blocking submit in those languages, it returns `false` only
  on temporary backpressure.
- A submit failure that is not temporary backpressure must be delivered
  as an exception.
- Returning a status code (`int`, `number`, and so on) is forbidden.

### Per-Language Naming Consistency
- A naming convention must not be mixed within one binding.
  - Python: every public API uses `snake_case` (including properties).
  - Java: `camelCase` methods, `PascalCase` classes.
  - C#: `PascalCase` throughout.
  - Go: `PascalCase` exported identifiers.
  - Rust: `snake_case` methods, `PascalCase` types.
  - C++: `snake_case` methods. Keep type naming consistent within one
    binding. A `_t` suffix can be used for a handle/value wrapper type,
    or where a type name would otherwise collide with a method name, but
    it is not forced onto every enum/class. Do not add a separate alias
    to the same type just to match a suffix rule.
  - Node/TypeScript: `camelCase` methods, `PascalCase` classes.

### Full C API Coverage
- Each per-language spec file must describe a binding interface that
  covers every `ZLINK_EXPORT` function in `core/include/zlink.h` and the
  public headers under `core/include/zlink/**`, without omission.
- The mapping does not have to be 1:1 (for example, a group of option
  functions can consolidate into a single typed facade).
- But no C API capability may be missing from a binding spec.
- When a new C API is added to a public header, every per-language spec
  file must be updated together.
## Service Layer Policy
- This section defines the public API policy for the service layer
  (Spot, Actor) that sits on top of the socket layer.
- The service layer follows the same POSD principles, naming policy,
  error policy, ownership policy, and testing policy as the socket
  layer.
- The service layer's baseline is the Spot/Actor C API in
  `core/include/zlink.h`.

### Spot / SpotNode Lifecycle (POSD Principles)

- **`SpotNode` is the lifecycle owner.** `Spot` is a pub/sub facade on
  top of it, valid only while `SpotNode` is alive.
- `Spot` is not built with an independent constructor. **It is created
  through a factory method such as `SpotNode.createSpot(...)`**. The
  name follows language idiom (`spot_node.new_spot`,
  `spotNode.createSpot`, and so on).
- The "get if it exists, otherwise create" flow keyed on an explicit
  Spot routing id is exposed through a `SpotNode.getOrCreateSpot(...)`
  family method that directly maps to
  `zlink_spot_node_spot_get_or_new(...)`. A binding must not emulate
  this meaning by combining lookup and create.
- `Spot`'s life is bound to its parent `SpotNode`.
  - `spot.close()` — ends only the Spot; the node stays alive
  - `spotNode.close()` — cleans up the node and every live Spot under it
    together (cascading close)
- This removes the need for a user to manually combine the close order
  of `Spot` and `SpotNode`. The binding pre-processes child spots inside
  `SpotNode.close()` before tearing down the node.
- The C API's raw `zlink_spot_new(...)` + `zlink_spot_node_new(...)`
  combination is not exposed as a binding public constructor as-is. It
  must be wrapped in a `SpotNode`-centered factory pattern.

### Service Layer Introspection Surface Tiers

The service layer's introspection/snapshot/entry types are **split into
two tiers by usage frequency**. A binding spec reflects this split.

- **Primary (core)**: a snapshot/query surface an ordinary user uses
  frequently. Described in `bindings/<lang>/README.md`'s upper section.
  - `SpotNodeStatus` (spot node status)

- **Advanced/Diagnostic**: for special purposes such as debugging or
  operational monitoring. Described in a separate "Advanced" or
  "Diagnostic" subsection in the spec.
  - `SpotNodePeerEntry`, `SpotNodeSubjectEntry`
  - `SpotNodeSocketEntry`, `SpotNodeSpotEntry`, `SpotNodeActorEntry`
  - Various filter types (`SpotNodePeerFilter`, `SpotNodeSubjectFilter`,
    `SpotNodeSocketFilter`)

The Primary types alone must be enough for a basic usage scenario. The
"register / discover / connect a service" flow must complete without
learning the Advanced types.

### Public Exposure Of `zlink_errno()`

- A binding **does not expose the raw `zlink_errno()`/`zlinkErrno()`
  function publicly**. Error detail is accessed **only ever through the
  error type's `internalErrno`/`internal_errno` field**.
- Do not create a dual path where a user investigating an error
  "sometimes uses `ZlinkException.getCode()` and sometimes uses
  `Zlink.errno()`" — unify it to one entry point.
- It's allowed for a binding's internal implementation to call
  `zlink_errno()` to fill in the exception object (for internal
  interpretation). The ban applies only to the public surface.
- A message-lookup utility such as `Zlink.strerror(errno)` can remain as
  a convenience, but the raw `errno()` accessor should be private or
  removed.

### Service Layer Architecture
- The service layer's current public axes are `SpotNode`, `Spot`,
  `Actor`, `StreamSocket`'s Actor binding surface, and the SPOT route
  bridge/publisher surface. The public Discovery/Registry handle was
  removed from the core contract in core 8.4.3, so it must not be
  revived as a new binding surface.

```
SpotNode
  |-- bind
  |-- raw mesh: connectPeer, disconnectPeer
  |   createPublisher
  |-- actor: create, lookup, remote create, join, leave
  |-- introspection: status, peers, peers(filter),
  |   subjects, spots, actors
  `-- TLS: setTlsServer, setTlsClient

Spot
  |-- publish, subscribe
  |-- sendToChannel, requestToChannel
  |-- sendToSpot, requestToSpot, requestToRouter
  |-- replyToSpot, replyToRouter
  |-- actor join: recvActorJoin, replyActorJoin, actors
  |-- actor lifecycle: recvActorLifecycle
  |-- setSubscription, unsetSubscription
  |-- setDispatchHandler, setSendReadyHandler
  `-- close facade only

Actor
  |-- ref: nodeRid, actorId, generation
  |-- receive: recvPart
  |-- bound session: send, close
  `-- close lifecycle handle

StreamSocket
  |-- bindActor, unbindActor
  `-- sendBoundActor

  |-- connect
  |-- snapshot
  `-- close
```

### Actor Dispatch Policy

Actor dispatch is a formal service-layer contract that currently exists
in core's public header. A binding does not hide Actor as an internal
SPOT detail — it organizes it as a separate public capability spanning
`SpotNode`, `Spot`, `Actor`, and `StreamSocket`.

If the language provides a unit for splitting public surface — a header,
module, package, namespace — Actor must have its own independent
entrypoint. This entrypoint must not be a thin forwarding file that
merely re-includes/imports/exports the whole SPOT header or module. The
Actor entrypoint must substantively own the public types and function
declarations that make up the Actor contract — the Actor value object,
the Actor lifecycle handle, the Actor recv/join helper. A structure where
the SPOT entrypoint reuses the Actor entrypoint is allowed, but a
structure where the Actor entrypoint exists only by leaning on the whole
SPOT implementation is non-compliant.

The baseline core public types and functions are as follows.

- Types: `zlink_actor_ref_t`, `zlink_actor_route_t`,
  `zlink_actor_recv_info_t`, `zlink_actor_join_info_t`,
  `zlink_actor_join_result_t`, `zlink_actor_join_entry_spot_result_t`,
  `zlink_actor_lookup_result_t`, `zlink_spot_actor_lifecycle_info_t`,
  `zlink_actor_join_spot_handler_fn`,
  `zlink_actor_join_entry_spot_handler_fn`,
  `zlink_actor_lookup_handler_fn`, `zlink_spot_node_spot_entry_t`,
  `zlink_spot_node_actor_entry_t`
- `SpotNode` axis: `zlink_spot_node_actor_new`,
  `zlink_spot_node_actor_lookup`, `zlink_remote_actor_get_ref` (async
  lookup), `zlink_spot_node_actor_destroy` (async submit),
  `zlink_spot_node_actor_join_spot` (async submit + a dedicated
  completion typedef), `zlink_spot_node_actor_join_entry_spot` (async
  submit + a dedicated completion typedef),
  `zlink_spot_node_actor_leave_spot` (async submit),
  `zlink_spot_node_actor_recv_part`,
  `zlink_spot_node_actor_send_bound_session_msg`,
  `zlink_spot_node_actor_reply_no_bind`,
  `zlink_spot_node_actor_close_bound_session`
- `Spot` axis: `zlink_spot_actor_join_recv`, `zlink_spot_actor_join_reply`,
  `zlink_spot_recv_actor_lifecycle`, `zlink_spot_actors`
- `StreamSocket` axis: `zlink_stream_bind_actor` (async submit),
  `zlink_stream_unbind_actor` (async submit),
  `zlink_stream_send_bound_actor_part`, `zlink_stream_bound_actors`
- Snapshot axis: `zlink_spot_node_spots`, `zlink_spot_node_actors`,
  `zlink_spot_actors`

The binding surface follows this split of responsibility.

| Public owner | Actor role |
|---|---|
| `SpotNode` | Local Actor create/lookup, async remote Actor lookup, async destroy, async join/leave, node-level Actor snapshot |
| `Actor` | Holds the Actor ref, Actor recv, bound STREAM session message send, bound session close |
| `Spot` | Actor join request recv/reply, Actor lifecycle event receive, a snapshot of Actors currently joined to this Spot |
| `StreamSocket` / session facade | Async STREAM session Actor bind/unbind, send targeted at a bound Actor, session-attach list lookup |

A binding must provide the following domain objects as a public
contract. The name can be converted to fit language convention, but the
field meaning does not change.

| Object | Required meaning |
|---|---|
| `ActorRef` | `node_rid`, `actor_id`, `generation` |
| `ActorRoute` | The routed target Actor, current Spot routing id, current Spot kind |
| `ActorRecvInfo` | The receiving Actor, source node/session routing id, flags |
| `ActorReceived` | `ActorRecvInfo` plus payload parts. The name can change per language convention, but the part-by-part loop and `has_more` are not exposed as public fields. In a language that owns the payload parts, expose it as a disposable envelope, not a cloneable record/value |
| `ActorJoinInfo` + join message | The `source_actor`, `target_actor`, `source_node_rid`, `source_spot_rid`, `target_node_rid`, `target_spot_rid`, `join_epoch`, `flags`, and join message needed to judge and respond to a join request. Can be grouped into an `ActorJoinRequest` wrapper or a tuple/pair per language convention. A wrapper that owns the join message must be disposable. The native reply context is kept only inside the binding and is not exposed as a public field |
| `ActorJoinResult` | Delivered on join completion. `result`, the final `actor` ref (the target node's ref for a remote join), `joined_spot_rid`, `join_epoch`, `flags` |
| `ActorJoinEntrySpotResult` | Delivered on Entry Spot join completion. `result`, the final `actor` ref, `target_node_rid`, `join_epoch`, `flags`. No join message or reply payload |
| `ActorLookupResult` | Delivered on remote Actor lookup completion. `result`, the checked `actor` ref, `flags` |
| `SpotActorLifecycleEvent` | The result of draining a Spot lifecycle readable event. `kind`, `info`. In a language where it also owns request parts, expose it as a disposable envelope, not a cloneable record/value |
| `SpotActorLifecycleInfo` | Included in a Spot lifecycle event. `previous_actor`, `current_actor`, `previous_spot_rid`, `current_spot_rid`, `join_epoch`, `flags` |
| `SpotNodeSpotEntry` | Spot routing id, Entry/User Spot kind, whether a dispatch handler is set, joined/pending Actor count, route sync state, change timestamp |
| `SpotNodeActorEntry` | Actor ref, current Spot routing id, current Spot kind, route sync state, pending message count, change timestamp |

The detailed rules are as follows.

- An Actor id is a non-empty UTF-8 string up to 255 bytes. A NUL
  character is not allowed.
- `generation == 0` is an unchecked remote ref, and is not treated as an
  invalid value.
- A local Actor is created by `SpotNode`, and its lifecycle handle is
  exposed as the per-language `Actor` type. An Actor can join only one
  Spot at a time.
- `leave` is an async submit API. It does not drain unread Actor
  messages. It always returns to the Entry Spot of the same node —
  if `leave` succeeds from a user Spot, a source-left event and an
  Entry-Spot-joined lifecycle event fire, and the active route is
  updated to the Entry Spot location.
- Entry Spot join is an async submit API. The target argument is the
  SpotNode rid, not an Entry Spot rid. Because a SpotNode has only one
  Entry Spot, the public API does not require a separate Entry Spot rid.
  An Entry Spot join does not send a join message and does not go
  through the application join queue. The completion handler returns
  only success/failure and the final Actor ref.
- An Actor that must start on a remote node is created by the
  application directly on that SpotNode with `actor_new`. A checked ref
  for a remote Actor is obtained through the async
  `remote_actor_get_ref` lookup. Remote create-or-get and an admission
  handler are not on the public surface.
- A Spot join request carries a message. A join reply must also return a
  message to the caller together with the accept/reject result. Join
  completion delivers the final Actor ref and joined Spot rid to the
  caller as an `ActorJoinResult` value.
- The request-reply surface exposes only the payload part the core reply
  function supports. Because the core reply function has no send-flag
  argument, a binding does not add a no-op flag-setting step to the
  reply builder.
- `ActorJoinInfo` exposing this does not mean it must expose every field
  of native `zlink_actor_join_info_t` as a public field. A per-language
  binding keeps the native request context needed for the reply as
  opaque internal state. The public value object exposes
  `source_actor`, `target_actor`, the source/target node and Spot
  routing id, `join_epoch`, `flags`, and the message — what a user needs
  to judge and respond.
- One STREAM session can bind multiple Actors. Bind/unbind is keyed on
  the session routing id and either the actor id or the Actor ref.
- When a language can naturally provide a session facade, it's better to
  expose STREAM Actor bind/unbind and send targeted at a bound Actor as
  operations on the session facade, rather than as socket-wide
  functions. This avoids repeatedly passing the session routing id.
- A public API that sends from STREAM to an Actor uses the bound session
  and actor id as its selector.
- Actor location is updated through the Actor creation, Spot join/leave,
  and Actor destroy flows. STREAM session bind/unbind does not change
  Actor location.
- There is no per-Actor queue-limit option. A binding must not make this
  a public option.
- A removed Actor ref function, a stream actor lookup/send helper, or a
  session-actor-key design name is not kept in the public surface or
  documentation.

An Actor dispatch event uses the same readiness model as the SPOT
dispatch event handler.

- `ZLINK_SPOT_DISPATCH_EVENT_ACTOR_READABLE` is a notification that an
  Actor part can be read. One callback does not mean one part.
- `ZLINK_SPOT_DISPATCH_SUBJECT_ACTOR`'s subject is a native Actor ref
  valid only during the callback. A binding's public API does not
  expose a raw pointer.
- A language that hands the callback off to a different execution
  context must non-blockingly pre-drain the Actor part at callback-entry
  time, so the public dispatch info can return that part.
- `ZLINK_SPOT_DISPATCH_EVENT_ACTOR_JOIN_READABLE` is the readiness signal
  for a Spot's Actor join request plane. A binding must let it be
  drained through `Spot.recvActorJoin` or an equivalent public surface
  until each language's no-data representation appears.

### SpotNode Capability Matrix

| Capability | SpotNode |
|---|---|
| `bind` | Y |
| `connectPeer` | Raw mesh only |
| `disconnectPeer` | Raw mesh only |
| `disconnectPeerRid` | Raw mesh only |
| `createSpot` | Y |
| `entrySpot` | Y |
| `spotLookup` | Y |
| `setTlsServer` | Y |
| `setTlsClient` | Y |
| `status` | Y |
| `peers` | Y |
| `peers(filter)` | Y |
| `subjects` | Y |
| `internalSockets` | Diagnostic |
| `spots` | Y |
| `actors` | Y |
| `close` | Y |

- SpotNode does not directly expose the data-plane API (`send`/`recv`/
  `publish`/`subscribe`).
- The data plane is accessed only through the `Spot` facade.
- `connectPeer`/`disconnectPeer` are control paths exclusive to raw peer
  topology.
- `createSpot` is a public factory placed on top of `zlink_spot_new()`.
  `entrySpot` wraps `zlink_spot_node_entry_spot()` in a per-language
  typed `Spot` factory. `spotLookup` wraps
  `zlink_spot_node_spot_lookup()` as a per-language typed `Spot` lookup
  surface. Treated as centered on `createPublisher`.

### Actor Capability Matrix

Actor dispatch is an independent service-layer capability spanning
`SpotNode`, `Actor`, `Spot`, and `StreamSocket`. Each binding must expose
the roles below as a public surface that fits its own language
convention.

| Capability | Public owner | Core substrate |
|---|---|---|
| local Actor create | `SpotNode` | `zlink_spot_node_actor_new` |
| local Actor lookup | `SpotNode` | `zlink_spot_node_actor_lookup` |
| unchecked remote Actor ref | `SpotNode` | `zlink_remote_actor_get_ref` |
| Actor destroy by ref | `SpotNode` | `zlink_spot_node_actor_destroy` |
| owned Actor close/destroy | `Actor` | `zlink_spot_node_actor_destroy` |
| Spot Actor lifecycle receive | `Spot` | `zlink_spot_recv_actor_lifecycle` |
| Actor join by ref | `SpotNode` | `zlink_spot_node_actor_join_spot` |
| Actor Entry Spot join by ref | `SpotNode` | `zlink_spot_node_actor_join_entry_spot` |
| owned Actor join | `Actor` | `zlink_spot_node_actor_join_spot` |
| owned Actor Entry Spot join | `Actor` | `zlink_spot_node_actor_join_entry_spot` |
| Actor leave by ref | `SpotNode` | `zlink_spot_node_actor_leave_spot` |
| owned Actor leave | `Actor` | `zlink_spot_node_actor_leave_spot` |
| Actor recv | `Actor` | `zlink_spot_node_actor_recv_part` |
| no-bind request reply | `SpotNode` | `zlink_spot_node_actor_reply_no_bind` |
| bound session send | `Actor` | `zlink_spot_node_actor_send_bound_session_msg` |
| bound session close | `Actor` | `zlink_spot_node_actor_close_bound_session` |
| join request recv | `Spot` | `zlink_spot_actor_join_recv` |
| join request reply | `Spot` | `zlink_spot_actor_join_reply` |
| STREAM bind Actor | `StreamSocket` / session facade | `zlink_stream_bind_actor` |
| STREAM unbind Actor | `StreamSocket` / session facade | `zlink_stream_unbind_actor` |
| STREAM send bound Actor | `StreamSocket` / session facade | `zlink_stream_send_bound_actor_part` |
| STREAM bound Actor snapshot | `StreamSocket` / session facade | `zlink_stream_bound_actors` |
| node Spot snapshot | `SpotNode` | `zlink_spot_node_spots` |
| node Actor snapshot | `SpotNode` | `zlink_spot_node_actors` |
| Spot joined Actor snapshot | `Spot` | `zlink_spot_actors` |

### Spot Capability Matrix

| Capability | Spot |
|---|---|
| `publish(topic, ...)` | Y |
| `subscribe` | Y |
| `receiveSubscriptionEvent` | Y |
| `setSubscription` / `unsetSubscription` | Y |
| `sendToChannel` / `requestToChannel` | Y |
| `sendToSpot` | Routed ordinary send (spot → spot) |
| `requestToSpot` | Routed request initiation (spot → spot) |
| `requestToRouter` | Routed request initiation (spot → router) |
| `replyToSpot` | Routed reply surface (spot → spot) |
| `replyToRouter` | Routed reply surface (spot → router) |
| `setDispatchHandler` | Y |
| `setSendReadyHandler` | Y |
| `recvActorLifecycle` | Y |
| `close` | Y |

- Spot is not a socket type — it's a channel-aware facade layered on top
  of SpotNode.
- Spot routed receive can be exposed as `recv_routed` or an equivalent
  typed recv surface.
- Spot has no `bind`/`connect` (SpotNode owns that).
- Spot's `close` releases only the facade — SpotNode stays alive.

### Removed Discovery/Registry Capability

The public Discovery and Registry C API was removed from the core
contract in core 8.4.3. A binding must not expose a Discovery/Registry
factory, resolver method, sync option, registry query client, or
compatibility alias as current API.

### Service Observability Policy
- Public service-layer observation uses a snapshot/query surface instead
  of a separate monitor handle.
- SPOT (SpotNode, Spot) observation uses the `status`, `peers`,
  `peers(filter)`, `subjects`, `spots`, and `actors` APIs. A binding that
  needs internal socket diagnostics keeps `internalSockets` as a
  separate diagnostic surface.
- When a state transition needs to be observed, compare successive
  snapshot/query results.
- The SocketMonitor callback release policy stays the same as before.
  - When a callback registration API exists, release it only through
    `close()`

### Service Layer Domain Objects
- The service layer must also use domain objects.
- The minimum core domain objects:
  - `MonitorStatus`: a monitor status snapshot
  - `SpotNodeStatus`: SpotNode status (state, peer count, and so on)
- Advanced/Diagnostic domain objects:
  - `SpotNodePeerEntry`: peer information
  - `SpotNodeSubjectEntry`: subject information
  - `SpotNodeSocketEntry`: internal socket diagnostic information. Uses
    the shared `SocketType` enum for the socket kind — does not create a
    separate SpotNode-only socket-type enum that repeats the same
    values.
  - `SpotNodeSpotEntry`: node-owned Spot information
  - `SpotNodeActorEntry`: node-owned Actor route information
- Filter objects:
  - `SpotNodePeerFilter`: a peer-lookup filter
  - `SpotNodeSubjectFilter`: a subject-lookup filter
  - `SpotNodeSocketFilter`: an internal socket diagnostic filter
- Enum/value objects:
  - `SocketType`: the socket kind shared between an ordinary socket and
    SpotNode's internal socket diagnostics
  - `SpotRole`: `PUB`, `SUB`
  - `SubjectKind`: `NONE`, `TOPIC`, `PATTERN`
  - `SpotNodeState`: `IDLE`, `CONNECTING`, `PARTIAL_READY`, `READY`,
    `ERROR`
  - `MonitorSourceKind`: `SOCKET`, `SPOT_PUB`, `SPOT_SUB`
  - `SpotPeerSource`: `MANUAL`, `DISCOVERY`, `MIXED`
  - `SpotPeerState`: `CONFIGURED`, `CONNECTING`, `CONNECTED`
- `MonitorStatus.isReady()` or an equivalent convenience accessor
  interprets ready meaning only for a raw socket monitor source. For a
  `SPOT_PUB`/`SPOT_SUB` source, the ready bit must not be
  reinterpreted as extended SPOT readiness.

### Service Layer Naming Policy
- The service layer also follows the Naming Policy.
- The allowed variation is the same three variations as the Naming
  Policy — casing variation, a minimal suffix for a language without
  overloads, and per-language property/getter convention only.
- Word substitution, omission, or replacement is forbidden.
- The detailed rules are the same as the Naming Policy body.

#### Service Layer Canonical Name Table

| Component | Canonical Name | Description |
|---|---|---|
| SpotNode | `bind` | Binds an endpoint |
| SpotNode | `connectPeer` | Connects a raw peer |
| SpotNode | `disconnectPeer` | Disconnects a raw peer |
| SpotNode | `createRouteBridge` | Registers a caller/channel-runtime-owned socket with the SPOT route bridge |
| SpotNode | `createPublisher` | Creates a publisher handle used for SpotNode's topic-publish ingress |
| SpotNode | `setTlsServer` | Configures TLS server |
| SpotNode | `setTlsClient` | Configures TLS client |
| SpotNode | `status` | A node status snapshot |
| SpotNode | `peers` | A peer-list snapshot |
| SpotNode | `peers(filter)` | A filtered peer lookup |
| SpotNode | `subjects` | A subject-list snapshot |
| SpotNode | `internalSockets` | An internal socket diagnostic snapshot |
| SpotNode | `spots` | A node-owned Spot snapshot |
| SpotNode | `actors` | A node-owned Actor snapshot |
| SpotNode | `close` | Terminates the node |
| Spot | `publish(topic, ...)` | Publishes a Spot topic |
| Spot | `subscribe` | Receives a topic subscription |
| Spot | `receiveSubscriptionEvent` | Receives a topic subscription event |
| Spot | `setSubscription` / `unsetSubscription` | Manages a subscription filter |
| Spot | `sendToChannel` / `requestToChannel` | A channel-targeted routed send/request |
| Spot | `setDispatchHandler` | Registers the topic/routed/channel-reply/timer readable notification handler |
| Spot | `setSendReadyHandler` | Registers the send-ready callback handler |
| Spot | `recvActorLifecycle` | Receives an Actor join/leave lifecycle event |
| Spot | `close` | Terminates the facade |

### Service Layer Test Policy
- Because the service layer includes components not directly verified
  by a sample or perf, it must be tested for correct FFI mapping,
  lifecycle, and type conversion.
- The service layer is tested using the same categories as the Test
  Matrix.

#### Service Layer Surface Tests
- Confirm SpotNode role-matrix alignment
- Confirm Spot role-matrix alignment
- Confirm the service TLS helper exists
- Confirm the typed domain objects exist (SpotNodeStatus,
  SpotNodePeerEntry, SpotNodeSocketEntry, SpotNodeSpotEntry,
  SpotNodeActorEntry, and so on)
- Confirm the typed enums exist (SpotRole, SubjectKind, SpotNodeState,
  and so on)

#### Service Layer Contract Tests
- SpotNode: no leak across the create/bind/close lifecycle
- Spot: create/close lifecycle (SpotNode must stay alive)
- Confirm native resources are cleaned up on exception/error paths too

#### Service Layer Behavior Tests
- SpotNode bind → Spot publish → Spot subscribe path succeeds
- Spot subscribe → returns empty when there's no data (non-blocking)
- Confirm exception on Spot publish failure
- Confirm the Spot dispatch event callback fires
- Confirm the Spot setSendReadyHandler callback fires
- Confirm the Spot receiveSubscriptionEvent path
- Confirm SpotRouteBridge attach/send/request/handleReceived paths work
- Confirm the SpotNode publisher handle publish path works

#### Service Layer Introspection Tests
- SpotNode status → verify SpotNodeStatus fields (state, peerCount,
  subjectCount, and so on)
- SpotNode peers → verify the SpotNodePeerEntry list
- SpotNode peers(filter) → verify the filtered result
- SpotNode subjects → verify the SpotNodeSubjectEntry list

#### Service Layer Test Scope

| Test Category | SpotNode+Spot | Actor | Stream Actor Binding |
|---|---|---|---|
| Surface | Required | Required | Required |
| Contract | Required | Required | Required |
| Behavior | Required | Required | Required |
| Introspection | Required | Required | Required |

- A binding without a service/spot family can exclude this test.
- Here, "monitor" refers to a socket monitor.

### Service Layer Sample Policy
- Service-family samples defined in the Canonical Sample Set:
  - `spot_recv_sample`: Spot channel-aware subscribe/routed recv
  - `spot_callback_sample`: Spot dispatch event callback
  - `monitor_recv_sample`: monitor event receive (including socket
    monitor)
- A binding without a service/spot family can exclude the `spot_*`
  samples.

### Per-Binding Service Layer Scope
- Not every binding has to implement the entire service layer.
- The minimum requirement:

| Component | Required level |
|---|---|
| SpotNode + Spot | Required if that binding has spot support |

### Callback API Policy
- A callback registration API is exposed according to each socket
  type's role.
- The Callback Capabilities table above is the baseline.
- Canonical handler registration names:
  - `setDispatchHandler`: registers the SPOT unified readable
    notification callback
  - `setSendReadyHandler`: registers the send-ready status callback
- SPOT routed receive and Actor lifecycle do not expose a direct
  callback registration API. `setDispatchHandler` announces a readable
  event, and the user explicitly drains the queue with `recvRouted` or
  `recvActorLifecycle`.
- `onReceive` may be used only as the internal name for the raw
  `STREAM` direct fragment callback. It is not used as a canonical
  public binding API name.
- Unregistering a callback by setting it to `null`/`None` is not
  allowed. A callback is unregistered only by closing the socket.

## Core API Additions

This section summarizes the core APIs added to `core/include/zlink.h`.
Each binding must expose these APIs as a per-language typed surface.

### Request-Reply Policy

> See `cpp/`, `java/`, `dotnet/`, `node/`, `python/`, `go/`, `rust/` for
> per-language interface signatures and usage examples.

#### Design Principles

- Request-reply is handled through the ZMP protocol envelope. It does
  not use a scheme that attaches a request marker to `zlink_msg_t`.
- Dispatch, the pending map, timeout, and reply matching are all handled
  in the core C API. A binding does not reimplement this logic.
- Core provides a callback-based async model. A binding can layer a
  per-language completion-object-return surface on top of the callback,
  per the
  [bindings async execution surface policy](async-coroutine-policy.md).
  A coroutine connection is the framework's responsibility.
- `request()` is not a thread-blocking API.
- Request-reply is a capability extension of the Router/Dealer sockets
  and SPOT — not a separate abstraction layer; it layers a role on top of
  the existing surface.

#### APIs Not On The Public Surface

The message-level request-reply marker API and the per-message metadata
API are not part of the public surface. A binding does not expose the
following functions or constants publicly, and does not keep request
marker state inside a `Message` object.

- `zlink_msg_set_request`, `zlink_msg_set_reply`,
  `zlink_msg_get_request_info`
- `zlink_msg_set_metadata`, `zlink_msg_get_metadata`,
  `zlink_msg_clear_metadata`

#### Valid Request-Reply Combinations

**Socket paths:**

| Requester | Responder | Possible | Reply path |
|--------|--------|------|-----------|
| Dealer | Router | Y | Router replies using the Dealer's routing_id |
| Router | Router | Y | Both sides reply using routing_id |
| Dealer | Dealer | **N** | Neither side has a routing_id |
| Router | Dealer | **N** | Dealer cannot reply to a specific peer |

**SPOT paths:**

| Requester | Responder | Possible | Reply path |
|--------|--------|------|-----------|
| Spot | Spot | Y | Replies with the peer's address + request_seq |
| Spot | Router | Y | Spot requests Router; Router replies to Spot |
| Router | Spot | Y | Router requests Spot; Spot replies to Router |

`DealerSocket.request()` connection constraints:
- Every connected target must be a Router. If Router and Dealer are
  mixed on a Dealer's connections, request can fail.
- A binding does not validate this constraint at runtime. It is the
  user's responsibility, and it is documented in the API docs.

#### C API Surface

**Shared type:**

```c
typedef void (*zlink_reply_handler_fn)(
    zlink_request_result_t result_,
    zlink_msg_t *parts,
    size_t part_count,
    void *userdata);

```

The `parts` delivered to a callback is a borrowed view. It's valid only
until the callback returns. Copy it to keep it beyond that.

**Socket API:**

```c
zlink_submit_result_t zlink_dealer_request_part(void *dealer,
    zlink_msg_t *part, zlink_send_flags_t flags,
    zlink_part_flag_t part_flag, uint32_t timeout_ms,
    zlink_reply_handler_fn handler, void *userdata);

zlink_submit_result_t zlink_dealer_reply_part(void *dealer,
    uint64_t request_seq, zlink_msg_t *part,
    zlink_part_flag_t part_flag);

zlink_submit_result_t zlink_router_request_part(void *router,
    const zlink_routing_id_t *peer_rid, zlink_msg_t *part,
    zlink_send_flags_t flags, zlink_part_flag_t part_flag,
    uint32_t timeout_ms, zlink_reply_handler_fn handler,
    void *userdata);

zlink_submit_result_t zlink_router_reply_part(void *router,
    const zlink_routing_id_t *peer_rid, uint64_t request_seq,
    zlink_msg_t *part, zlink_part_flag_t part_flag);

zlink_recv_result_t zlink_router_recv_part(void *router,
    const zlink_routing_id_t **source_node_rid_out,
    const zlink_routing_id_t **source_spot_rid_out,
    uint64_t *request_seq_out, zlink_msg_t *part_out,
    zlink_part_flag_t *has_more_out, zlink_recv_flags_t flags);
```

**SPOT API:**

```c
zlink_submit_result_t zlink_spot_send_channel_part(void *spot, ...);
zlink_submit_result_t zlink_spot_request_channel_part(void *spot, ...);
zlink_submit_result_t zlink_spot_send_spot_part(void *spot, ...);
zlink_submit_result_t zlink_spot_request_spot_part(void *spot, ...);
zlink_submit_result_t zlink_spot_request_router_part(void *spot, ...);
zlink_submit_result_t zlink_spot_reply_spot_part(void *spot, ...);
zlink_submit_result_t zlink_spot_reply_router_part(void *spot, ...);
zlink_submit_result_t zlink_router_request_spot_part(void *router, ...);
zlink_submit_result_t zlink_router_reply_spot_part(void *router, ...);
zlink_submit_result_t zlink_router_send_spot_part(void *router, ...);
zlink_submit_result_t zlink_spot_publish_part(void *spot, ...);
zlink_recv_result_t zlink_spot_subscribe_part(void *spot, ...);
zlink_recv_result_t zlink_spot_recv_part(void *spot, ...);
zlink_handler_result_t zlink_spot_dispatch_event_handler(void *spot, ...);
```

See `core/include/zlink.h` for the full signatures.

#### Receive Dispatch Model

Core handles request-reply dispatch. A binding does not implement a
dispatch owner.

- `request_seq = 0` means an ordinary message.
- `request_seq != 0` means a request-reply message.
- Core matches by `source_node_rid + request_seq` in the pending map.
- A reply that fails to match (a stray/late reply) is dropped.
- ROUTER uses the typed surface `zlink_router_recv_part()` instead of
  the generic `zlink_recv_part()`. Calling the generic
  `zlink_recv_part()` returns `EOPNOTSUPP`.
- ROUTER's routed receive plane is a **single surface**. Both ordinary
  ROUTER traffic and spot-origin routed traffic are received through the
  one `zlink_router_recv_part()`. A `NULL` `source_spot_rid` means
  ordinary ROUTER traffic; a filled-in one means spot-origin traffic.

#### Request API Variants

A request has two completion modes.

Both async request and callback-completion request are exposed through
the `RequestOp` operation builder the `request` entrypoint returns. The
per-completion-mode flags, timeout, and failure-delivery rules follow the
[bindings async execution surface policy](async-coroutine-policy.md).

The C binding keeps the substrate shape
`zlink_*_request_part(..., flags, part_flag, timeout, ...)`. The wrapper
builder policy does not apply to the C ABI.

- Error handling follows the Error Handling Policy. A callback request's
  submit failure applies language idiom as-is: an exception for
  exception languages (C++/Java/.NET/Node/Python), a returned error for
  return-based languages (C/Go/Rust).
- A reply result is delivered by the callback exactly once:
  `(RequestResult result, List<Message> parts)`

#### SPOT Request-Reply

The same request-reply protocol is used on top of SPOT direct delivery.
It's layered as `SPOT routed envelope -> request-reply envelope ->
payload`. A SPOT reply is also sent with the peer's address +
request_seq, without a ctx. Multiple requests can be outstanding
concurrently on the same Spot. A high-level request's completion ends
with the first reply.

#### Timeout

- Timeout is managed by core. A binding does not implement timeout
  logic.
- The default timeout is `5000ms`. Priority: per-call > socket default >
  the implementation default `5000ms`.
- `timeout_ms = 0` uses the socket's default timeout.
- Timeout applies to the total elapsed time — send wait plus reply
  wait combined.
- On timeout, core removes the entry from the pending map and delivers
  `ZLINK_REQUEST_TIMED_OUT` to the callback.
- Core drops a late reply that arrives after timeout.

#### Pending Map

- Assigning `request_seq`, pending registration, reply matching, and
  timeout removal are all done in core.
- A binding does not maintain a separate pending map.
- The only thing a binding maintains is the callback → Future/Promise
  resolve mapping.

#### Wire Format

- `request_seq` is an unsigned 64-bit integer (8 bytes, network byte
  order).
- The starting value is `1`. `0` is reserved to mean an ordinary
  message.
- On overflow, it wraps to `1`. A value that collides with an
  outstanding one is skipped.
- The envelope has 4 control parts: protocol id, version, message type,
  request_seq.
- When combined with SPOT routed, it's 8 SPOT control parts + 4
  request-reply control parts + payload.
- A binding does not parse the envelope directly. Core handles it.

#### Return Type

- On success, `request()` returns **only the reply payload's
  `List<Message>`** (a per-language list type such as `Vec<Message>`,
  `IReadOnlyList<Message>`, `Message[]`, or `tuple[Message, ...]`).
- The caller already knows the target routing_id and request_seq of the
  request it sent, so it does not need a `Received` wrapping that back.
- A separate `Reply` type is not created.
- Because multipart reply support is the goal, it's a list shape rather
  than a single `Message`. A single-part reply is retrieved with
  `parts[0]`.
- A request handler (server side) delivers `peer_rid`, `request_seq`,
  and the payload together. A separate `Request` type or a dedicated
  `onRequest` callback is not created. (This differs because the server
  side needs to know who sent which request_seq.)

#### Ownership

- Message ownership on a `request()`/`reply()` call follows the existing
  send contract.
- The `parts` delivered to a request callback is a borrowed view.
  Invalid after the callback returns. A binding copies it and delivers a
  per-language list type or `Vec<Message>`.
- On socket close, core rejects every outstanding request in the pending
  map with a `ZLINK_REQUEST_TERMINATED` callback.

#### Callback Contract

- The callback is called exactly once. On success it's delivered as
  `result = OK` plus reply parts; on failure, `result != OK` plus an
  empty/null/Err path.
- The core callback signature:
  `void(zlink_request_result_t result_, zlink_msg_t *parts_, size_t part_count_, void *userdata_)`
- Per-language patterns (inheriting the per-function `RequestError`):
  - C++: `std::function<void(request_result_t, std::vector<message_t>)>`
  - Java: `BiConsumer<RequestResult, List<Message>>`
  - .NET: `Action<RequestResult, IReadOnlyList<Message>>`
  - Node: `(result: RequestResult, parts: Message[]) => void`
  - Python: `callback(result: RequestResult, parts: list[Message])`
  - Go: `func(RequestResult, []*Message)` (nil/empty allowed on failure)
  - Rust: `FnOnce(Result<Vec<Message>, RequestError>)` (Rust idiom;
    `RequestError::code` maps to `RequestResult`)

### SPOT Messaging Policy

> See `cpp/`, `java/`, `dotnet/`, `node/`, `python/`, `go/`, `rust/` for
> the per-language SPOT interfaces.

The SPOT public surface separates two naming axes. `sendToChannel(...)`
and `requestToChannel(...)` are the channel-aware direct messaging path,
while `publish(topic, ...)` publishes to the topic plane the `Spot`
itself belongs to. Direct address-targeted routed messaging is an
optional supplementary typed surface. Request-reply is layered on top
of routed messaging.

#### Pub/Sub Messaging

SPOT pub/sub is a publish/subscribe model based on the channel a `Spot`
handle belongs to and a `topic`. The publish caller does not pass the
channel name as a separate argument.

```c
/* publish */
zlink_submit_result_t zlink_spot_publish_part(void *spot,
    const char *topic_id, zlink_msg_t *part, zlink_send_flags_t flags,
    zlink_part_flag_t part_flag);

/* subscribe receive */
zlink_recv_result_t zlink_spot_subscribe_part(void *spot,
    const zlink_routing_id_t **source_rid_out,
    char *topic_id_buf, size_t topic_id_capacity,
    size_t *topic_id_len_out, zlink_msg_t *part_out,
    zlink_part_flag_t *has_more_out, zlink_recv_flags_t flags);

/* subscription filter */
zlink_config_result_t zlink_set_subscription(
    void *handle,
    const char *filter);
zlink_config_result_t zlink_unset_subscription(
    void *handle,
    const char *filter);
```

Binding rules:
- The C API does not have a separate no-wait function name for publish.
- A non-blocking publish calls
  `zlink_spot_publish_part(..., ZLINK_DONTWAIT, ...)` and classifies
  errno into `zlink_submit_result_t`. A binding does not add a separate
  `tryPublish` or `publishNoWait`.
- A `subscribe` receive is exposed as a typed receive surface that
  returns `topic + parts`.
- Topic filter configuration is exposed as a typed subscription API.
- A channel-aware send/request or topic publish failure is promoted to
  `SubmitError`.
  - `NOT_FOUND`: for channel-aware send/request, no matching
    `channel_name` or attach target exists; for topic publish, there is
    no target on the topic plane to publish to.
  - `NOT_CONNECTED`: an attachment exists, but there is no active/
    send-ready path
  - `BACKPRESSURED`: a path exists, but the HWM has been reached
  - `NOT_ADMITTED`: the target peer is draining, so a new submit is
    rejected

#### Routed Direct Messaging

SPOT routed direct messaging sends a message directly to a specific Spot
or Router peer, or a routed reply target. The core substrate is
expressed through the part-based C functions below. Both the high-level
binding's `Spot` facade and `RouterSocket`'s router-to-spot helper expose
this capability as an operation builder start point that fits the
`Operation Builder Policy`. A raw socket's ordinary send/request/reply
also follows the same builder pattern.

```c
/* spot -> spot */
zlink_submit_result_t zlink_spot_send_spot_part(void *spot,
    const zlink_routing_id_t *dest_node_rid,
    const zlink_routing_id_t *dest_spot_rid,
    zlink_msg_t *part, zlink_send_flags_t flags,
    zlink_part_flag_t part_flag);

/* router -> spot */
zlink_submit_result_t zlink_router_send_spot_part(void *router,
    const zlink_routing_id_t *dest_node_rid,
    const zlink_routing_id_t *dest_spot_rid,
    zlink_msg_t *part, zlink_send_flags_t flags,
    zlink_part_flag_t part_flag);
```

Binding rules:
- The C ABI keeps a part-based functional contract.
- The high-level binding's `Spot` endpoint, `RouterSocket`'s
  router-to-spot helper, and every ordinary send/request/reply/publish
  surface on raw `DealerSocket`/`RouterSocket`/`PubSocket`/`StreamSocket`
  all follow this document's `Operation Builder Policy`.
- The destination address/request sequence is taken as a builder
  start-point argument, and payload/flags/timeout/callback are expressed
  as builder steps.
- Routed recv uses the handler/recv surface of the Event Dispatcher
  below.

#### SPOT Lifecycle / Bridge / Deprecated Attachment

```c
void *zlink_spot_new(void *node);          /* create SPOT facade */
zlink_close_result_t zlink_spot_destroy(void **spot_p);

void *zlink_spot_node_new(
    void *ctx,
    const zlink_spot_node_options_t *options);
zlink_close_result_t zlink_spot_node_destroy(void **node_p);
zlink_bind_result_t zlink_spot_node_bind(void *node, const char *endpoint);
zlink_connect_result_t zlink_spot_node_connect_peer(void *node,
    const char *peer_endpoint);
zlink_connect_result_t zlink_spot_node_disconnect_peer(void *node,
    const char *peer_endpoint);
zlink_connect_result_t zlink_spot_node_disconnect_peer_rid(void *node,
    const zlink_routing_id_t *peer_rid);

void *zlink_spot_route_bridge_new(
    void *ctx,
    void *spot_node,
    const zlink_spot_route_bridge_options_t *options);
int zlink_spot_route_bridge_attach_router_channel(
    void *bridge,
    const char *channel_name,
    void *router,
    const zlink_spot_route_bridge_endpoint_options_t *options);
int zlink_spot_route_bridge_send(
    void *bridge,
    const char *channel_name,
    const zlink_routing_id_t *target_node_rid,
    const zlink_routing_id_t *target_spot_rid,
    zlink_msg_t *parts,
    size_t part_count,
    zlink_send_flags_t flags);
int zlink_spot_route_bridge_request(
    void *bridge,
    const char *channel_name,
    const zlink_routing_id_t *target_node_rid,
    const zlink_routing_id_t *target_spot_rid,
    zlink_msg_t *parts,
    size_t part_count,
    zlink_reply_handler_fn reply_handler,
    void *userdata,
    zlink_send_flags_t flags,
    uint32_t timeout_ms);
int zlink_spot_route_bridge_handle_router_received(
    void *bridge,
    const char *channel_name,
    const zlink_routing_id_t *source_node_rid,
    uint64_t request_seq,
    zlink_msg_t *parts,
    size_t part_count,
    bool *handled_out);
int zlink_spot_route_bridge_drain(void *bridge);
int zlink_spot_route_bridge_close(void *bridge);

void *zlink_spot_node_publisher_new(void *node);
int zlink_spot_node_publisher_publish(
    void *publisher,
    const char *topic,
    zlink_msg_t *parts,
    size_t part_count,
    zlink_send_flags_t flags);
int zlink_spot_node_publisher_close(void *publisher);
```

`options == NULL` or `options->mode == 0` turns on every SPOT
capability. A binding uses this default in each language's default
constructor, and where it exposes `mode`, it maps `PUBSUB`, `ROUTED`, and
`ALL` to the same meaning as the C contract. The internal socket
observation API is based on `zlink_spot_node_internal_sockets()`, and
returns only already-created sockets.

The SpotNode option facade must not omit core's six public options.

| Core option | Binding surface |
|-------------|-----------------|
| `ZLINK_SPOT_NODE_OPT_ROUTER_HWM_PROFILE` | router admission HWM profile |
| `ZLINK_SPOT_NODE_OPT_ROUTER_HWM` | router admission HWM override |
| `ZLINK_SPOT_NODE_OPT_PUBSUB_HWM_PROFILE` | pub/sub admission HWM profile |
| `ZLINK_SPOT_NODE_OPT_PUBSUB_HWM` | pub/sub admission HWM override |
| `ZLINK_SPOT_NODE_OPT_DISPATCH_WORKERS_MIN` | minimum dispatch callback workers |
| `ZLINK_SPOT_NODE_OPT_DISPATCH_WORKERS_MAX` | maximum dispatch callback workers |

Dispatch worker min/max configures `SpotNode`'s dispatch-callback
execution pool. It must not be described as an option that changes the
data-plane thread count or transport I/O thread count. Value validation
matches core: `min >= 1`, `max >= min`. A binding exposes these two
values as a per-language typed option/property, and must not revive a
raw option bag as the canonical path.

Binding rules:
- `SpotNode` and `Spot` are exposed as separate typed handles.
- `Spot` is a facade layered on top of `SpotNode`. When `SpotNode` is
  released, `Spot` also becomes invalid.
- Use `SpotRouteBridge` to send from Spot to a different channel, or to
  receive a Spot relay packet on a `ROUTER` channel. The `ROUTER` socket
  registered with the bridge continues to be owned by the caller or the
  channel runtime.
- A raw Core socket has no API to set or query logical channel metadata.
  A channel name is used only as the logical routing value
  `SpotRouteBridge`'s typed operations accept.
- The bridge's `handle_router_received()` is called from the channel
  runtime's receive loop. When `handled == true`, the bridge takes
  payload ownership, and the caller does not process the same received
  object again.
- `SpotNodePublisher` is a handle for publishing to SpotNode's topic
  publish ingress without external code attaching a raw `PUB` socket to
  `SpotNode`.
- `Spot.publish(topic).message(...).submit()` is the channel-aware topic
  plane that enters `SpotNode`'s own topic publish ingress queue. An
  external channel call is described through `SpotRouteBridge` and the
  channel-runtime-owned socket path.
- `connect_peer`/`disconnect_peer` are control paths exclusive to raw
  peer topology. They must not be described as the central API of the
  channel-aware public surface.

### SPOT Event Dispatcher Policy

Core provides a callback-based event dispatcher model. It can handle
multiple event sources (sub recv, routed recv, timer, send-ready)
without synchronization, inside a single I/O thread context.

Core principles:
- Once a handler callback is registered, the core I/O thread calls the
  callback when the event occurs.
- Because every callback runs in the same thread context, state can be
  shared without a lock.
- Calling recv, send, or reply inside the callback has no
  synchronization issue.
- The timer also runs in the same context.

#### Callback Registration API

```c
/* raw STREAM direct recv callback */
zlink_handler_result_t zlink_recv_handler(void *s,
    zlink_socket_msg_handler_fn handler, void *userdata);

/* raw STREAM packet callback */
zlink_handler_result_t zlink_stream_packet_handler(void *stream,
    zlink_stream_packet_handler_fn handler, void *userdata);

/* register writable notification callback */
zlink_handler_result_t zlink_send_ready_handler(void *s,
    zlink_send_ready_handler_fn handler, void *userdata);
```

Rules:
- A core C attach function allows only one active handler per subject.
  Attaching again while a native handler is already attached can return
  `EBUSY`. A public binding's `set...Handler` surface does not directly
  repeat this raw attach function — it provides the meaning of storing
  or replacing the current public handler.
- `zlink_recv_handler()` is allowed only on raw `STREAM`.
- `zlink_stream_packet_handler()` is also allowed only on raw `STREAM`,
  and the three modes — `recv`/raw callback/packet callback — are
  mutually exclusive.
- Raw `PAIR`, `DEALER`, `ROUTER`, `SUB`, `XSUB` do not have a direct
  receive callback install surface. `PAIR`, `DEALER`, `ROUTER` receive
  only through a public recv method, and `SUB`, `XSUB` receive only
  through the topic subscribe receive surface.
- After a callback is registered, a direct recv on the same subject and
  registering that data-plane `ZLINK_POLLIN` can fail with `EBUSY`. The
  exact scope follows STREAM/SPOT's per-type rules.
- A public callback setter is replace-only. Passing `NULL` is not
  allowed.

#### Spot Dispatch Event Handler

Spot's core event dispatcher is `zlink_spot_dispatch_event_handler()`.
Once this handler is registered, every event related to the Spot arrives
through a single callback. Callbacks for the same `spot` must be
delivered in order. The implementation must not call the same `spot`'s
dispatch callback concurrently or reentrantly. Inside the callback, a
caller must be able to check the event kind and process Spot messaging
sequentially by calling recv.

This serialization is per-`spot`. It does not require global
serialization across different `spot`s. The implementation must be able
to process different Spots in parallel, while still preserving the
sequential-processing contract for the same `spot`.

```c
typedef enum zlink_spot_dispatch_event_t {
    ZLINK_SPOT_DISPATCH_EVENT_SUBSCRIBE_READABLE = 1,
    ZLINK_SPOT_DISPATCH_EVENT_ROUTED_READABLE = 2,
    ZLINK_SPOT_DISPATCH_EVENT_TIMER_READABLE = 3,
    ZLINK_SPOT_DISPATCH_EVENT_CHANNEL_REPLY_READABLE = 4,
    ZLINK_SPOT_DISPATCH_EVENT_ACTOR_READABLE = 5,
    ZLINK_SPOT_DISPATCH_EVENT_ACTOR_JOIN_READABLE = 6
} zlink_spot_dispatch_event_t;

typedef enum zlink_spot_dispatch_subject_kind_t {
    ZLINK_SPOT_DISPATCH_SUBJECT_SPOT = 1,
    ZLINK_SPOT_DISPATCH_SUBJECT_TIMER = 2,
    ZLINK_SPOT_DISPATCH_SUBJECT_CHANNEL_DEALER = 3,
    ZLINK_SPOT_DISPATCH_SUBJECT_ACTOR = 4
} zlink_spot_dispatch_subject_kind_t;

typedef struct zlink_spot_dispatch_info_t {
    zlink_spot_dispatch_event_t event;
    zlink_spot_dispatch_subject_kind_t subject_kind;
    void *subject;
} zlink_spot_dispatch_info_t;

typedef void (*zlink_spot_dispatch_event_handler_fn)(
    void *spot, const zlink_spot_dispatch_info_t *info, void *userdata);

zlink_handler_result_t zlink_spot_dispatch_event_handler(void *spot,
    zlink_spot_dispatch_event_handler_fn handler, void *userdata);
```

Usage pattern:
- Register a dispatch event handler.
- When the callback fires, check `info->event`, `info->subject_kind`,
  and `info->subject`.
- Inside the active dispatch callback for the same `spot`, the ordinary
  recv surfaces can be used.
- On `SUBSCRIBE_READABLE`, drain the pub/sub plane with
  `zlink_spot_subscribe_part()` or
  `zlink_spot_recv_subscription_event()`.
- On `ROUTED_READABLE`, recv the routed/request message with
  `zlink_spot_recv_part()`.
- On `TIMER_READABLE`, recv the timer fire with `zlink_timer_recv()`
  against the `info->subject` timer handle.
- `CHANNEL_REPLY_READABLE` is only a readiness signal — there is no
  separate public drain API. The reply is delivered automatically by
  core through the `zlink_reply_handler_fn` registered when
  `zlink_spot_request_channel_part()` was called. The `info->subject`
  dealer handle is diagnostic information meaningful only on the
  deprecated dealer attach path.
- On `ACTOR_READABLE`, drain `zlink_spot_node_actor_recv_part()` keyed on
  the Actor subject delivered via `info->subject`. The public API does
  not expose the raw subject pointer or the part loop — it returns an
  `ActorReceived` or an equivalent aggregate value object.
- On `ACTOR_JOIN_READABLE`, drain the join request plane with
  `zlink_spot_actor_join_recv()`.
- A dispatch event is a readable notification. One callback does not
  mean one message.
- Inside the callback, a caller must be able to drain that plane until
  there's nothing left to read.
- The first call to `zlink_spot_recv_part()` must not perform hidden
  activation, hidden queue open, or hidden registration.
- Because dispatch callbacks for the same `spot` are serialized, Spot
  messaging can be processed sequentially.
- Because different `spot`s can be processed in parallel, a
  high-performance room execution model can be built.

#### Spot Timer API

A Spot-owned timer is created with `zlink_spot_timer_new(spot)`, and
controlled afterward through the common `zlink_timer_*` functions.

```c
void *zlink_spot_timer_new(void *spot);

/* use the common timer API after creation */
zlink_close_result_t zlink_timer_destroy(void **timer_p);
zlink_config_result_t zlink_timer_start(void *timer,
    uint64_t interval_ns, uint64_t repeat_count);
zlink_config_result_t zlink_timer_stop(void *timer);

typedef void (*zlink_timer_handler_fn)(
    void *timer, uint64_t fire_count, void *userdata);

zlink_handler_result_t zlink_timer_handler(void *timer,
    zlink_timer_handler_fn handler, void *userdata);
zlink_recv_result_t zlink_timer_recv(void *timer, uint64_t *fire_count_out);
```

Rules:
- A timer is created dependent on a Spot, via
  `zlink_spot_timer_new(spot)`.
- After creation, it's controlled through the common
  `zlink_timer_start`, `zlink_timer_stop`, `zlink_timer_recv`,
  `zlink_timer_handler`, and `zlink_timer_destroy` APIs.
- `interval_ns` is in nanoseconds. `repeat_count = 0` means infinite
  repeat.
- A timer fire arrives at the dispatch event handler as
  `TIMER_READABLE`.
- A timer handler callback can be registered directly, or polled with
  `zlink_timer_recv()`.
- Inside the dispatch callback, a pending fire can be processed
  sequentially with `zlink_timer_recv()`.

Binding rules:
- A timer is exposed as a typed wrapper.
- `interval_ns` is converted to that language's Duration type.
- Timer and dispatch event are unified, so a user must be able to handle
  sub recv + routed recv + timer without synchronization, just by
  registering a callback.

#### Dispatch Model Summary

```
zlink_spot_dispatch_event_handler callback
  (serialized per spot, non-reentrant)
  |-- SUBSCRIBE_READABLE -> zlink_spot_subscribe_part()
  |                         or zlink_spot_recv_subscription_event()
  |-- ROUTED_READABLE -> zlink_spot_recv_part()
  |-- TIMER_READABLE -> zlink_timer_recv()
  |-- CHANNEL_REPLY_READABLE -> readiness only; reply handler runs internally
  |-- ACTOR_READABLE -> zlink_spot_node_actor_recv_part()
  `-- ACTOR_JOIN_READABLE -> zlink_spot_actor_join_recv()
```

For the same `spot`, a caller must be able to process recv, send, and
reply sequentially inside this callback. Different `spot`s must be able
to run in parallel where needed. Inside the callback, a caller must be
able to drain the plane the event announced.

#### Receive-Model Summary

| Socket type | Receive path |
|-----------|----------|
| `PAIR` / `DEALER` | The runtime uses `zlink_recv_part()`; the public surface is aggregate recv |
| `SUB` / `XSUB` | The runtime uses `zlink_subscribe_part()`; the public surface is aggregate topic recv |
| `ROUTER` | The runtime uses `zlink_router_recv_part()`; the public surface is aggregate routed recv. Request completion stays on `zlink_reply_handler_fn` |
| `STREAM` | One of three modes below (mutually exclusive). Raw recv / `zlink_recv_handler()` / `zlink_stream_packet_handler()` |
| `SPOT` | `zlink_spot_recv_part()` + `zlink_spot_subscribe_part()` + `zlink_spot_recv_subscription_event()` + `zlink_spot_recv_actor_lifecycle()` + `zlink_spot_dispatch_event_handler()`. Does not expose a direct routed callback |

A binding reflects the contract above in its implementation as-is. A
public socket class exposes only the aggregate recv surface, and a
forbidden callback install surface must not be reachable by a bypass
through any base class.

#### Typed Receive Surface

SPOT receive provides several typed surfaces. A binding layers a
per-language handler/callback surface on top of these typed surfaces.

#### Spot Receive

```c
zlink_recv_result_t zlink_spot_recv_part(void *spot, ...);
zlink_recv_result_t zlink_spot_recv_actor_lifecycle(void *spot, ...);
```

- `request_seq = 0` means an ordinary routed message.
- `request_seq != 0` means a request-reply message.
- `source_rid + spot_rid` is the sender's address, used as the reply
  target.
- A binding's public API exposes an aggregate `Received` or a
  per-language equivalent type, instead of the part helper.
- Actor lifecycle is drained with `zlink_spot_recv_actor_lifecycle()`
  after a dispatch event.

#### Router Receive (unified routed recv surface)

```c
zlink_recv_result_t zlink_router_recv_part(void *router,
    const zlink_routing_id_t **source_node_rid_out,
    const zlink_routing_id_t **source_spot_rid_out,
    uint64_t *request_seq_out,
    zlink_msg_t *part_out, zlink_part_flag_t *has_more_out,
    zlink_recv_flags_t flags);
```

- ROUTER's routed receive is a single plane. Both ordinary ROUTER
  traffic and spot-origin routed traffic are received through one recv.
- `source_spot_rid == NULL` means ordinary ROUTER traffic (reply uses
  `zlink_router_reply_part`). A filled-in `source_spot_rid` means
  spot-origin traffic (reply uses `zlink_router_reply_spot_part`).
- `request_seq == 0` means fire-and-forget. `request_seq != 0` means a
  request.
- A binding does not separately expose a ROUTER data-plane callback
  install surface. The request completion callback stays only on the
  `request(...)` path.

#### Pub/Sub Receive

- Raw `SUB`, `XSUB` are receive-only topic sockets.
- A binding exposes a per-language aggregate topic receive surface on
  top of the `zlink_subscribe_part()` typed receive substrate.
- A direct topic callback install surface is not placed on the raw
  pub/sub family.

#### SPOT Snapshot Query

```c
zlink_config_result_t zlink_spot_node_status(void *node,
    zlink_spot_node_status_t *out);
zlink_config_result_t zlink_spot_node_peers(void *node,
    zlink_spot_node_peer_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_peers(void *node,
    const zlink_spot_node_peer_filter_t *filter,
    zlink_spot_node_peer_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_subjects(void *node,
    const zlink_spot_node_subject_filter_t *filter,
    zlink_spot_node_subject_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_internal_sockets(void *node,
    const zlink_spot_node_socket_filter_t *filter,
    zlink_spot_node_socket_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_spots(void *node,
    zlink_spot_node_spot_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_actors(void *node,
    zlink_spot_node_actor_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_actors(void *spot,
    zlink_actor_ref_t *entries, size_t *count);
```

Binding rules:
- A snapshot result is converted into a per-language typed domain object
  array.
- A filter query is exposed as a typed filter builder or struct.
- A binding must properly release the memory of a returned array.

### SpotNode Node-Level Options

SpotNode's node-level options are handled through the
`zlink_set_spot_node_option()` family.

## Option Policy

### Public Option Surface
- **A public raw `setOption(key, value)`/`getOption(key)` bag is
  forbidden.**
- **A public raw `setsockopt`/`getsockopt` bag is also forbidden.**
- Common options are exposed only through a per-language typed surface
  (facade).
- Specialized options are also exposed only through a per-language role
  surface (facade).
- If a spec still has a public path that rotates a raw enum key plus a
  generic setter/getter, that's a policy violation. (A C contract such
  as `set_option(ZLINK_OPT_*, value)` must not surface as binding public
  API. A native call path used internally by the binding is allowed.)
- Once a typed facade exists, **the raw path is not exposed
  redundantly** — a user should never have to choose between the two.
- Examples:
  - Java/.NET: `CommonSocketOptions`, `RouterSocketOptions`
  - Go: typed method set, role interface
  - Rust: typed builder, method set, newtype
  - Python/Node: property, namespace object, role object, typed method
    set

#### Option Facade Canonical Type Names
- Every binding must provide the canonical facade types below.
- The type name varies only in language casing convention.

| Facade | Contents | Applies to |
|---|---|---|
| `CommonSocketOptions` | linger, sendHighWaterMark, receiveHighWaterMark, sendTimeout, receiveTimeout, immediate, connectTimeout, ipv6, tcpNoDelay, tcpKeepAlive, heartbeatInterval/Ttl/Timeout, maxMessageSize, backlog, reconnectInterval/Max, submitRetryMode, submitRetryTimeout, submitRetryAttempts | All |
| `RouterSocketOptions` | mandatory (bool), handover (bool), probe (bool), connectRoutingId (RoutingId), requestTimeout (Duration), peerWeight (int, read/write) | Router |
| `DealerSocketOptions` | probe (bool), requestTimeout (Duration), peerWeight (int, read/write) | Dealer |
| `StreamSocketOptions` | notify (bool) | Stream |
| `PubSocketOptions` | verbose (bool), verboser (bool), noDrop (bool), manual (bool) | Pub, XPub |
| `SubSocketOptions` | topicsCount (int, read-only) | Sub, XSub |

- Each facade's option items are based on the matching option enum value
  in `core/include/zlink.h`.
- The option value type inside a facade follows the Option Value Types
  policy.
- A submit retry option exposes off/0ms/0 attempts as the default on the
  raw socket facade. A managed SPOT/service internal profile may use
  `LOCAL_FAILURE`/100ms/2 attempts, but this does not change the raw
  socket option default. A `DONTWAIT` call, backpressure, admission
  rejection, and the reply timeout after a successful request submit are
  not subject to submit retry.

### Option Value Types
- Expose an option value as a meaning-based type wherever possible.
- Policy:
  - a `0/1` option: `boolean`
  - a finite state set: `enum`
  - time meaning: `Duration` or that language's standard time type
  - a binary identifier: a value object such as `RoutingId`
  - a genuinely numeric setting: `int`/`long`
  - string/bytes: `String`/`byte[]`
- An option whose name alone is an enum while the value is a raw `int`
  is not sufficient.

## Performance Policy
- Performance is not a separate optimization item — it's part of the
  public API design.
- The canonical hot path must be the path with the fewest hidden costs.
- The following are forbidden by default on the hot path:
  - a hidden payload copy
  - a hidden array/list reallocation
  - unnecessary UTF-8 encoding/decoding
  - redundant wrapping at the binding layer
  - unnecessary boxing/unboxing just to build a result
- A convenience API must be documented if it costs more than the default
  path.
- The callback path and the direct receive path must not diverge
  excessively — not just in payload shape, but in cost model too.
- If a zero-copy, borrowed, or owned path differs, the cost model must
  be documented together with ownership.
- The intensity of performance verification can vary by language and
  runtime characteristics.
- Still, every binding must adopt, as a baseline policy, reducing
  unnecessary copies, allocations, and conversions on the hot path.

### High-Performance Buffer Ecosystem Policy (Recommended)
- The canonical public contract keeps `Message`/`List<Message>`/
  `Received`/`TopicMessage` as its baseline.
- On the send/publish/request/reply input path, however, it's
  recommended to support, as an adapter surface, **a buffer ecosystem
  type that is effectively standard in that language and gives a large
  copy-reduction benefit**.
- This support does not replace the canonical contract.
  - It does not change a recv result into an external library type.
  - It does not change a domain object's field type into an external
    library type.
  - Even where supported, it's limited to an entrypoint such as
    `Message` construction, an input adapter, a `from_*` helper, or
    `impl IntoMultipart`.
- Criteria for support:
  - Is it widely used in that language's networking/IO ecosystem?
  - Is the zero-copy or copy-reduction benefit substantial?
  - Does it avoid forcing dependency on a specific framework across the
    entire public surface?
- Not criteria:
  - a niche library
  - a buffer type used mainly inside a specific company/project
  - a wrapper that tries to replace the canonical type

Recommended priority:

| Language | Recommended support | Level | Notes |
|---|---|---|---|
| Java | Netty `ByteBuf` | Recommended | Very common in network stacks; the direct/off-heap path has significant value |
| Java | Agrona `DirectBuffer` | Optional | Useful in low-latency contexts, but lower priority than Netty |
| .NET | `ReadOnlyMemory<byte>` / `ReadOnlySequence<byte>` / `IBufferWriter<byte>` | Recommended | The standard buffer ecosystem; large copy-reduction benefit |
| .NET | `PipeReader` / `PipeWriter` | Optional | Useful for the `System.IO.Pipelines` user base |
| Rust | `bytes::Bytes` / `BytesMut` | Recommended | Effectively standard in the async/network ecosystem |
| Python | buffer protocol / `memoryview` | Recommended | Secures a zero-copy input path beyond `bytes`/`bytearray` |
| Node | `Buffer` / `Uint8Array` | Baseline | Effectively the default supported category |
| Go | `[]byte` / `[][]byte` | Baseline | The language's default path is already the hot-path standard |

- Design rules:
  - An adapter must be an input-side convenience. It does not change the
    canonical return type.
  - A **third-party buffer type** that is not the language's standard
    library or runtime should be split into a separate extension module
    rather than the core binding, where possible. For example, Java's
    `ByteBuffer` can stay in core, but Netty's `ByteBuf` belongs in a
    separate Netty extension.
  - Adapter support must not excessively widen the overload surface.
    Absorb it, where possible, into **one unified entrypoint** such as
    `MessageLike`, `IntoMultipart`, or the buffer protocol.
  - Even when accepting an external buffer type, the binding must
    clearly define the ownership/retain/release rules in documentation.
  - A user must not be left to guess a framework-specific object
    lifetime rule (`ByteBuf.retain/release`, a pooled buffer, and so
    on).
  - Do not confuse "can be supported" with "zero-copy is guaranteed."
    When a zero-copy guarantee isn't possible, document the possibility
    of a copy.

### Codec/Serializer Extension Module Policy
- `Message` and multipart transport itself remain the canonical binding
  core contract.
- protobuf/json/messagepack codec-aware domain conversion is treated as
  **a formal, separate extension contract layered on top of the binding
  core**.
- The `C` binding is the exception. `C` keeps the raw transport contract
  as its default public surface, and does not require codec-aware
  domain conversion as part of the default binding contract.
- So it may expose a helper such as `Parse(...)`, `Serialize(...)`,
  `ToMessage(...)`, or `FromMessage(...)` publicly. This helper must not
  be mixed into the binding core package/module, however.
- Required rules:
  - The binding core package/module must be codec-agnostic.
  - The binding core must not pull in a protobuf/json/messagepack
    dependency as a required dependency.
  - The `C` binding only needs to keep the raw byte/message contract
    formal, and has no obligation to add a protobuf/json helper as a
    public contract.
  - Every binding except `C` keeps the codec extension layer as a public
    contract, and must support the three codecs `protobuf`, `json`,
    `messagepack`.
  - For every binding except `C`, the `protobuf`, `json`, and
    `messagepack` extensions must each be provided as **a distribution
    unit separate from the core binding**.
  - A third-party buffer adapter extension follows the same principle.
    It must be provided as a distribution unit separate from the core
    binding, and the core binding must not require that extension
    dependency.
  - A codec extension can depend on the binding core, but the binding
    core must not depend on a codec extension.
  - Even once a codec extension is added, the canonical recv/request/
    reply contract stays based on `Message`, `List<Message>`,
    `Received`, `TopicMessage`.
  - A codec extension defines only the object <-> `Message` encode/
    decode helper contract. It's allowed to take a parser, schema, or
    generated-type input needed for the payload type.
  - A codec extension can add a helper that turns a transport result
    type into a domain object, but it must not replace the raw
    transport contract itself.
  - A codec extension document does not define packet-name inference
    rules, high-level outbound serializer lookup, or a typed
    request/reply decode policy.
  - In a language that has a framework, the framework documentation
    owns the policy above. The codec extension document describes only
    the low-level encode/decode helper's input conditions.
- Reasons:
  - To avoid forcing a specific codec dependency on a raw-transport
    user.
  - Because codec-ecosystem choice differs by language, to keep the
    binding core from locking onto one implementation.
  - To separate the high-level domain helper from the low-level
    transport ownership contract, reducing change amplification.

JSON codec baseline by language:

| Language | JSON baseline |
|---|---|
| C | none required |
| C++ | `nlohmann/json` |
| .NET | `System.Text.Json` |
| Java | `Jackson` |
| Node | built-in `JSON.parse` / `JSON.stringify` |
| Python | stdlib `json` |
| Go | `encoding/json` |
| Rust | `serde_json` |

- This table means "the implementation treated as the default when
  exposing a json codec extension publicly."
- Additional support for a different json library is possible. But the
  public contract, samples, tests, and default-behavior baseline follow
  the table above.
- On Node, the built-in JSON is the baseline for plain-object encode/
  decode, and typed validation can be layered on top of a separate
  schema/parser object.

MessagePack codec baseline by language:

| Language | MessagePack baseline |
|---|---|
| C | none required |
| C++ | `msgpack-c` |
| .NET | `MessagePack for C#` |
| Java | `jackson-dataformat-msgpack` |
| Node | `@msgpack/msgpack` |
| Python | `msgpack` |
| Go | `vmihailenco/msgpack/v5` |
| Rust | `rmp-serde` |

Bindings no longer define a codec extension distribution unit.

| Language | Core binding root | Binding-owned codec package policy |
|---|---|---|
| C | `bindings/c/include/zlink/`, `bindings/c/src/` | None |
| C++ | `bindings/cpp/include/zlink/` | None. Framework serialization is handled in `framework/languages/cpp/extensions/` |
| .NET | `bindings/dotnet/src/Zlink/` | None. Framework serialization is handled in `framework/languages/dotnet/src/` |
| Java | `bindings/java/src/main/java/systems/zlink/` | None. Framework serialization is handled in `framework/languages/java/` |
| Node | `bindings/node/src/` | None. Framework serialization is handled in `framework/languages/node/packages/` |
| Python | `bindings/python/src/zlink/` | None. Keeps only raw `Message`/bytes |
| Go | `bindings/go/` | None. Keeps only raw `Message`/bytes |
| Rust | `bindings/rust/src/` | None. Keeps only raw `Message`/bytes |

- Placement rules:
  - Do not mix codec helper source directly into the same directory as
    the core socket/message namespace.
  - A per-language codec spec document explains the raw-only policy, and
    if that language is a framework target, points to the framework
    codec extension's location.
  - Binding samples and tests verify raw `Message`/bytes behavior.

### External Buffer Attach/Release Hook Policy
- The C API's `zlink_msg_init_data(..., zlink_free_fn*, hint)` provides
  the capability to **attach an external buffer plus a release hook**.
- A binding exposes this capability publicly **only when it fits that
  language's idiom and memory model**.
- Base principles:
  - **A copy-based `Message` construction path is Required in every
    binding.**
  - **A VM- or GC-based language (Java, .NET, Go, Python, Node) does not
    provide a public API that hands a VM-managed buffer to the native
    queue borrowed/zero-copy.**
  - **A borrowed zero-copy wrap API without a release hook does not
    belong on a managed language's public surface, default send path, or
    perf-only fast path.**
  - A VM language's performance path must build a native-owned `Message`,
    fill its payload, and hand it to the part-based send/recv API —
    rather than lending the caller's buffer to the native queue.
  - External buffer attach is allowed **only when the release point can
    be closed by the public contract.**
- Allowed:
  - C++
    - Allows external attach in a shape such as
      `from_external(..., zlink_free_fn*, hint)`.
    - Because the release hook is explicit, it can be closed by the
      public contract.
- Discouraged/forbidden:
  - Java / .NET / Go / Rust / Python / Node
    - A generic public borrowed wrap (`wrapDirect`, `wrapNative`,
      `wrap_buffer`, and so on) is forbidden.
    - A send/publish/request/reply fast path that hands a VM-managed
      buffer to the native queue via
      `zlink_msg_init_data(..., NULL, NULL)` is forbidden.
    - A public or default fast path that pins a VM-managed buffer and
      then releases it via a release callback is forbidden.
    - Reason: it's hard to safely close, through a public contract, the
      backing buffer's lifetime after send, retain/release, arena/
      session, and its interaction with the GC.
- Exception:
  - Only a language where the caller explicitly owns the release hook
    and lifetime, like C++, can offer advanced external attach.
  - Adding this exception in a VM- or GC-based language first requires a
    separate draft spec, a public lifetime contract, a regression test,
    and a perf comparison. It is not added directly to the formal spec
    and implementation.

## Boundary Cost Policy
- Prefer performing boundary validation once, at the earliest safe
  location.
- If the same validation repeats across multiple layers, the reason must
  be clear.
- A value going into a fixed-size native struct returns an immediate
  error instead of truncating.
- A boundary value such as a string, topic, routing id, or metadata must
  consider all of the following together:
  - the length limit
  - the encoding cost
  - the copy count
  - the reallocation policy
- Length limits for binding input that map to core's fixed-size struct
  fields:

  | Field | C struct size | Binding validation responsibility |
  |------|--------------|----------------|
  | `RoutingId` | `data[255]` | Returns an immediate error when constructing the value object if it exceeds 255 bytes |
  | topic / filter | a C string (null-terminated) | The binding returns an immediate error if it contains an embedded null character. Core handles the length limit, so the binding does not separately validate length |
  | channel_name | `char[256]` | Returns an immediate error if it exceeds 255 bytes |
  | endpoint | `char[256]` | Returns an immediate error if it exceeds 255 bytes |
  | metadata | `zlink_msg_t` (variable) | Handled by core; the binding validates only null |

- When a value going into a fixed-size field exceeds the limit, a
  binding returns an immediate exception/error without truncation.
- Avoid unnecessary intermediate collection construction when building a
  public domain object.
- A helper or sample must not make a slow path look like the canonical
  path.

## Peer Weight Policy

Peer weight is the canonical surface that controls the peer-level
outbound selection ratio and drain state. Every binding must expose this
for the handles it implements.

Core API/contract:
- `ZLINK_ROUTER_OPT_WEIGHT`
- `ZLINK_DEALER_OPT_WEIGHT`
- Value range `0..10000`, default `100`
- The submit result `ZLINK_SUBMIT_NOT_ADMITTED` (value 13) — returned
  when the target peer's weight is `0`
- The socket monitor event `ZLINK_EVENT_PEER_WEIGHT_CHANGED` (bit 15)
- `zlink_spot_node_peer_entry_t.weight` /
  `zlink_member_peer_entry_t.weight`

Binding rules:
- `weight` is exposed through a per-language typed option/property
  surface. It applies to `ROUTER` and `DEALER`. A weight-setting surface
  is not exposed on `SpotNode` or `Spot`.
- Include `NOT_ADMITTED` in the `SubmitError` family so a caller can
  distinguish a weight-`0` rejection.
- The `PEER_WEIGHT_CHANGED` event bit is exposed as a typed value on the
  existing socket monitor/service monitor surface. `value` is the new
  weight, `0..10000`.
- The `SpotNodePeerEntry`/`MemberPeerEntry` domain object must include a
  `weight` field.

## Monitor Policy
- The monitor plane also follows the same rules.
- A public monitor receive is provided as a single `recv()`.
  - Blocking/non-blocking is controlled by a flags parameter or
    per-language convention.
- A monitor event is separate from the data plane, but the way
  blocking/non-blocking is distinguished must be the same.
- Monitor is a separate plane for observing a socket's state changes,
  readiness changes, and lifecycle events.
- A monitor payload must not be confused with the message data-plane
  payload.
- A monitor event type must be exposed as a typed event surface or an
  equivalent meaning-carrying surface.
- A monitor consumer must be able to read the event's meaning, not just
  a raw integer mask.
- The monitor lifecycle's relationship to the observed socket's
  lifecycle must be explainable:
  - when the monitor opens
  - when the monitor closes
  - what happens after the observed socket closes
- Monitor is not an API that replaces the data plane.
- A monitor's readiness/state event meaning must not conflict with the
  data-plane contract.
- A monitor sample and test must show:
  - the successful event-receive path
  - the non-blocking empty path
  - the relationship between a socket state change and a monitor event

## Error Policy

### Binding Validation vs Native Error
- The binding immediately blocks a format/range error in an input
  value.
- Core decides socket-state, connection-state, transport-state, and
  protocol-state errors, and the binding delivers them to the caller
  as-is.

### What A Binding Must Validate
- a value that risks truncation
- a value that risks overflow
- a value going into a fixed-size native struct
- a value with an obvious length limit
- an offset/length range error
- a non-nullable argument
- a value outside an enum's range

For these, use a binding exception.
- Java: `IllegalArgumentException`, `IndexOutOfBoundsException`,
  `NullPointerException`
- .NET: `ArgumentException`, `ArgumentOutOfRangeException`,
  `ArgumentNullException`
- Go: an immediate `error` return, or `panic` (a programmer error)
- Rust: a compile-time guarantee (`NonZero`, newtype), or
  `panic!`/`Result<T, E>`

### What Native Decides
- no peer
- backpressure
- insufficient readiness
- a conflict between callback mode and direct recv
- a socket type/state/runtime problem
- a transport, TLS, endpoint, or protocol error

For these, a binding converts the native error into that language's
idiom and delivers it to the caller. An exception language throws; a
return-based language returns an error value.
- C++: `throw zlink_error_t`
- Java: `throw ZlinkException`
- .NET: `throw ZlinkException`
- Node: `throw ZlinkError` (extends `Error`)
- Python: `raise ZlinkError` (extends `Exception`)
- Go: `return err` (`ZlinkError` or an equivalent typed error)
- Rust: `Err(E)` (`Result<T, E>`; `ZlinkError` only when multiple
  function families mix)

### Error Code Table

The codes zlink uses and their meaning. A binding maps these codes onto
its per-language error type, so a caller can distinguish the cause.

Codes split into two layers.

1. **Public result enum codes (0–706)** — the return enum value of a
   public C API function. A binding faces these directly and must
   expose them as a per-language error type. See
   [core/errno-map.md](https://kairos-code-dev.github.io/zlink/en/spec/core/04-errno-map/)
   for the full definition.
2. **Internal errno** — the internal raw errno looked up with
   `zlink_errno()`. Used to look up the detailed cause behind a coarse
   bucket such as `INTERNAL_ERROR`. A binding exposes this value through
   an `internalErrno`/`internal_errno` field (for debugging only).

#### Public Result Enum Catalog

A binding must map **every value, without omission**, of the 8 enums
below into a per-language representation. OK (0) is shared by every enum
and is not treated as an error.

##### `zlink_submit_result_t` (send, request submit, reply submit)

| Value | Constant | Internal errno | Category | Meaning |
|----|------|-----------|------|------|
| 0 | `OK` | — | success | submit succeeded |
| 1 | `BACKPRESSURED` | `EAGAIN` | control flow | the send queue is saturated (HWM) |
| 2 | `NOT_CONNECTED` | `ENOTCONN`, `EHOSTUNREACH` | control flow | the target peer/path is not connected |
| 3 | `NOT_FOUND` | `ENOENT` | control flow | the target peer/spot/route does not exist |
| 13 | `NOT_ADMITTED` | `ECONNREFUSED` family | control flow | a new submit was rejected because the target peer's weight is `0` |
| 4 | `TERMINATED` | `ETERM` | runtime/lifecycle | the context has terminated |
| 5 | `INVALID_HANDLE` | `EFAULT` | caller contract violation | a NULL handle / invalid pointer |
| 6 | `INVALID_ARGUMENT` | `EINVAL` | caller contract violation | an invalid argument |
| 7 | `NOT_SUPPORTED` | `ENOTSUP` | caller contract violation | not supported on that socket type |
| 8 | `INVALID_STATE` | `EFSM`, `EBUSY` | caller contract violation | a socket/handle state error |
| 9 | `THREAD_VIOLATION` | `EMTHREAD` | caller contract violation | accessed from the wrong thread |
| 10 | `OUT_OF_MEMORY` | `ENOMEM` | internal failure | a memory allocation failure |
| 11 | `SEQ_EXHAUSTED` | `EBUSY` | internal failure | the request seq space is exhausted |
| 12 | `INTERNAL_ERROR` | `EPROTO`, and so on | internal failure | an internal submit failure (see `zlink_errno()` for detail) |

##### `zlink_request_result_t` (request completion callback)

| Value | Constant | Internal errno | Meaning |
|----|------|-----------|------|
| 0 | `OK` | `0` | successfully received the reply payload |
| 101 | `TIMED_OUT` | `ETIMEDOUT` | reply did not arrive within `timeout_ms` |
| 102 | `NOT_FOUND` | `ENOENT` | target not found; completed with an error reply |
| 103 | `TERMINATED` | `ETERM` | (reserved) an explicit termination completion path |
| 104 | `PROTOCOL_ERROR` | `EPROTO` | the reply envelope or error reply payload is corrupt |
| 105 | `INTERNAL_ERROR` | `EPROTO`, and so on | an internal request failure (see `zlink_errno()` for detail) |
| 106 | `REJECTED` | `EACCES`, `ECONNREFUSED` | the target explicitly rejected the request |
| 107 | `CONFLICT` | `ESTALE` | a conflict on the request target or state |
| 108 | `BUSY` | `EBUSY` | the request-processing path is temporarily busy |
| 109 | `NOT_CONNECTED` | `ENOTCONN`, `EHOSTUNREACH` | the target peer/path is not connected |
| 110 | `INVALID_ARGUMENT` | `EINVAL`, `EFAULT` | a request argument or envelope error |
| 111 | `INVALID_STATE` | `EFSM` | the handle state cannot accept a request |
| 112 | `NOT_SUPPORTED` | `ENOTSUP`, `EOPNOTSUPP` | request is not supported on this target |

##### `zlink_recv_result_t` (recv, subscribe, subscription event, monitor recv, timer recv)

| Value | Constant | Internal errno | Meaning |
|----|------|-----------|------|
| 0 | `OK` | — | receive succeeded |
| 201 | `NO_DATA` | `EAGAIN` | non-blocking recv has no data / the source is exhausted |
| 202 | `BUSY` | `EBUSY` | a handler is already attached |
| 203 | `TERMINATED` | `ETERM` | the context has terminated |
| 204 | `INVALID_HANDLE` | `EFAULT` | a NULL / invalid handle |
| 205 | `NOT_SUPPORTED` | `ENOTSUP` | recv is not supported on this socket type |
| 206 | `INTERNAL_ERROR` | `EPROTO`, and so on | an internal recv failure (see `zlink_errno()` for detail) |

##### `zlink_handler_result_t` (handler registration)

| Value | Constant | Internal errno | Meaning |
|----|------|-----------|------|
| 0 | `OK` | — | handler registration succeeded |
| 301 | `INVALID_ARGUMENT` | `EINVAL` | a NULL handler |
| 302 | `BUSY` | `EBUSY` | a handler is already attached |
| 303 | `NOT_SUPPORTED` | `ENOTSUP` | an unsupported subject |
| 304 | `DEADLOCK` | `EDEADLK` | a reentrant call (send-ready handler only) |
| 305 | `INVALID_HANDLE` | `EFAULT` | a NULL / invalid handle |
| 306 | `INTERNAL_ERROR` | `EPROTO`, and so on | an internal handler-registration failure (see `zlink_errno()` for detail) |

##### `zlink_close_result_t` (close, destroy)

| Value | Constant | Internal errno | Meaning |
|----|------|-----------|------|
| 0 | `OK` | — | close/destroy succeeded |
| 401 | `BUSY` | `EBUSY` | an in-flight callback / API call |
| 402 | `SHUTDOWN` | `ESHUTDOWN` | already closed |
| 403 | `INVALID_HANDLE` | `EFAULT` | a NULL / invalid handle |
| 404 | `INTERNAL_ERROR` | `EPROTO`, and so on | an internal close failure (see `zlink_errno()` for detail) |

##### `zlink_bind_result_t` (bind)

| Value | Constant | Internal errno | Meaning |
|----|------|-----------|------|
| 0 | `OK` | — | bind succeeded |
| 501 | `INVALID_ARGUMENT` | `EINVAL` | an invalid endpoint |
| 502 | `ADDR_IN_USE` | `EADDRINUSE` | the address is already in use |
| 503 | `NOT_SUPPORTED` | `ENOTSUP` | an unsupported transport |
| 504 | `INVALID_HANDLE` | `EFAULT` | a NULL / invalid handle |
| 505 | `INTERNAL_ERROR` | `EPROTO`, and so on | an internal bind failure (see `zlink_errno()` for detail) |

##### `zlink_connect_result_t` (connect, disconnect, unbind)

| Value | Constant | Internal errno | Meaning |
|----|------|-----------|------|
| 0 | `OK` | — | connect/disconnect/unbind succeeded |
| 601 | `INVALID_ARGUMENT` | `EINVAL` | an invalid endpoint |
| 602 | `NOT_SUPPORTED` | `ENOTSUP` | an unsupported transport |
| 603 | `INVALID_HANDLE` | `EFAULT` | a NULL / invalid handle |
| 604 | `INTERNAL_ERROR` | `EPROTO`, and so on | an internal connect/disconnect failure (see `zlink_errno()` for detail) |
| 605 | `NOT_FOUND` | `ENOENT` | the endpoint or peer routing id does not exist |
| 606 | `CONFLICT` | `EADDRINUSE` | the peer routing id conflicts with two or more pipes |
| 607 | `BUSY` | `EBUSY` | the lifecycle owner rejected a manual change |

##### `zlink_config_result_t` (option set/get, message lifecycle, snapshot, poller mutation, proxy, timer config)

| Value | Constant | Internal errno | Meaning |
|----|------|-----------|------|
| 0 | `OK` | — | configuration succeeded |
| 701 | `INVALID_HANDLE` | `EFAULT` | a NULL / invalid handle |
| 702 | `INVALID_ARGUMENT` | `EINVAL`, `EBUSY` | an invalid argument, or a config-layer conflict |
| 703 | `NOT_SUPPORTED` | `ENOTSUP` | an unsupported option |
| 704 | `INTERNAL_ERROR` | `EPROTO`, and so on | an internal config failure (see `zlink_errno()` for detail) |
| 705 | `INVALID_STATE` | `EBUSY`, `ESHUTDOWN` | the lifecycle state rejects the config |
| 706 | `NOT_FOUND` | `ENOENT` | the local lookup target does not exist |

##### Non-OK Value Total

- **59** non-OK codes in total (submit 13 + request 12 + recv 6 +
  handler 6 + close 4 + bind 5 + connect 7 + config 6 = 59). The value
  ranges are: 1–13, 101–112, 201–206, 301–306, 401–404, 501–505,
  601–607, 701–706.
- Because the value ranges don't overlap across enums, a single `int`
  uniquely identifies the code.
- A binding must provide a per-language error representation for all 59
  values. If one is missing, the caller has no way to distinguish that
  cause.

See the `Per-Language ErrorCode Mapping` section below for per-language
enum/constant mapping style.

#### Standard POSIX errno

On a platform where POSIX doesn't define the matching constant, a
`ZLINK_HAUSNUMERO`-based substitute value is used. A binding must compare
by constant name and must not depend directly on the integer value.

| errno | Substitute value (when POSIX doesn't define it) | Meaning | Representative situation |
|-------|-------------------------|------|--------------|
| `ENOTSUP` | HAUSNUMERO + 1 | an unsupported operation | an operation impossible on that socket type |
| `EPROTONOSUPPORT` | HAUSNUMERO + 2 | protocol not supported | a request for an unsupported protocol |
| `ENOBUFS` | HAUSNUMERO + 3 | insufficient buffer space | an internal buffer allocation failure |
| `ENETDOWN` | HAUSNUMERO + 4 | the network is down | a transport-layer failure |
| `EADDRINUSE` | HAUSNUMERO + 5 | the address is already in use | an endpoint conflict on bind |
| `EADDRNOTAVAIL` | HAUSNUMERO + 6 | the address cannot be used | an invalid endpoint format |
| `ECONNREFUSED` | HAUSNUMERO + 7 | the connection was refused | the target refused the connection |
| `EINPROGRESS` | HAUSNUMERO + 8 | the operation is in progress | an async connect in progress |
| `ENOTSOCK` | HAUSNUMERO + 9 | not a socket target | an invalid handle was passed |
| `EMSGSIZE` | HAUSNUMERO + 10 | the message size was exceeded | the message exceeds the configured max size |
| `EAFNOSUPPORT` | HAUSNUMERO + 11 | address family not supported | an unsupported address family |
| `ENETUNREACH` | HAUSNUMERO + 12 | the network is unreachable | routing is impossible |
| `ECONNABORTED` | HAUSNUMERO + 13 | the connection was aborted | the connection ended abnormally |
| `ECONNRESET` | HAUSNUMERO + 14 | the connection was reset | the peer forcibly closed the connection |
| `ENOTCONN` | HAUSNUMERO + 15 | not connected | a send/recv attempt before connecting |
| `ETIMEDOUT` | HAUSNUMERO + 16 | the operation timed out | a request-reply timeout, a connect timeout |
| `EHOSTUNREACH` | HAUSNUMERO + 17 | the target is unreachable | the peer isn't connected; routing is impossible |
| `ENETRESET` | HAUSNUMERO + 18 | the network was reset | the network connection dropped |
| `EAGAIN` | (POSIX standard) | the resource is temporarily unavailable | the HWM was reached on a non-blocking send (backpressure) |
| `EINVAL` | (POSIX standard) | an invalid argument | out of range, an invalid option value |
| `ECANCELED` | (POSIX standard) | the operation was canceled | the caller canceled the request |

The `ZLINK_HAUSNUMERO` value is `156384712`.

#### zlink-Specific errno

zlink's own error codes. Uses a `ZLINK_HAUSNUMERO`-based offset so it
does not collide with POSIX errno.

| Substitute value | Constant | Meaning | Representative situation |
|--------|------|------|--------------|
| HAUSNUMERO + 51 | `EFSM` | a finite-state-machine error | an operation not allowed in the socket's state (for example, direct recv in callback mode) |
| HAUSNUMERO + 52 | `ENOCOMPATPROTO` | an incompatible protocol | a peer connection using a different protocol version |
| HAUSNUMERO + 53 | `ETERM` | context/socket terminated | an operation attempted while the context or socket is closed |
| HAUSNUMERO + 54 | `EMTHREAD` | insufficient I/O threads | the context's I/O threads are insufficient |

#### Per-Language ErrorCode Mapping

Each binding maps the Public Result Enum Catalog's 59 non-OK codes onto
a per-language enum/constant, providing type-safe branching.

| Language | Handling | ErrorCode type | Access |
|------|------|---------------|----------|
| C | return | a per-function typed enum (`zlink_*_result_t`) | the return value itself |
| C++ | throw | a unified `ErrorCode` enum | `zlink_error_t.code()` |
| Java | throw | a unified `ErrorCode` enum | `ZlinkException.getCode()` |
| .NET | throw | a unified `ErrorCode` enum | `ZlinkException.Code` |
| Node | throw | a unified `ErrorCode` enum (or string constant) | `ZlinkError.code` |
| Python | throw | a unified `ErrorCode` enum | `ZlinkError.code` |
| Go | return | a unified `ErrorCode` typed int constant | `ZlinkError.Code()` |
| Rust | return (`Result`) | a unified `ErrorCode` enum variant | `ZlinkError.code()` |

- Each variant of the unified enum maps 1:1 to one of the Public Result
  Enum Catalog's 59 values. A binding can either keep the original C
  enum split (submit / recv / handler / close / bind / connect / config
  / request), or unify it into a single enum per language idiom. Either
  style must **express every value without omission**.
- A constant/variant name can either keep the original
  `UPPER_SNAKE_CASE` or convert to that language's style
  (`PascalCase`/`camelCase`). The numeric value and meaning are fixed.
- An `internalErrno`/`internal_errno` field is provided separately,
  mainly for looking up the detailed cause behind a coarse bucket such
  as `INTERNAL_ERROR`.

### Request-Reply Error Policy

Request-reply uses two subtypes from the Per-Function Error Type
Hierarchy: **`RequestError`** (request completion) and **`SubmitError`**
(request submit). `RequestError` maps to `zlink_request_result_t`, and
`SubmitError` maps to `zlink_submit_result_t`.

Error codes split into two layers.

**Wire error reply codes** — a protocol-level error reply the peer
sends. Only 3 errno values are usable on the wire: `ENOENT`,
`EOPNOTSUPP`, `EINVAL`.

**API/completion codes** — the errno core delivers to the callback:

| errno | When it occurs |
|-------|----------|
| `ENOENT` | the target peer/spot could not be found (wire or local) |
| `EOPNOTSUPP` | a peer-kind mismatch, or not supported |
| `EINVAL` | an invalid parameter |
| `ETIMEDOUT` | the reply wait exceeded the timeout |
| `EPROTO` | envelope parse failure, or an invalid remote reply |
| `EBUSY` | a receive-surface conflict (duplicate handler registration) |

**Request errors (`RequestError`):**

| Situation | `request()` |
|------|------------|
| backpressure | waits for writable (added to the timeout) |
| timeout | `RequestError(TIMED_OUT)` |
| target not found | `RequestError(NOT_FOUND)` |
| a remote error reply | `RequestError(<matching code>)` |
| the socket closed | `RequestError(TERMINATED)` |
| a protocol error | `RequestError(PROTOCOL_ERROR)` |
| a reply not in the pending map | ignored |

**Reply errors (`SubmitError`):**

| Situation | `reply()` |
|------|-----------|
| success | returns normally |
| backpressure | `SubmitError(BACKPRESSURED)` |
| not connected | `SubmitError(NOT_CONNECTED)` |
| any other failure | `SubmitError(<matching submit code>)` |

- An async request delivers a completion failure through the async
  completion path (a Future reject / an await error).
- A callback request **throws/returns a submit failure immediately**,
  and delivers only a post-submit-success completion failure through the
  callback's `RequestResult`/`RequestError`.
- Uses the per-function-family subtype error (see Per-Function Error
  Type Hierarchy).
  - a submit failure: `SubmitException`/`SubmitError`
  - a request completion failure: `RequestException`/`RequestError`
- Per-language representation:
  - Java: `SubmitException`/`RequestException` — distinguish the cause
    with `getCode()` (unchecked)
  - .NET: `ZlinkSubmitException`/`ZlinkRequestException` — the `Code`
    property
  - Node: `SubmitError`/`RequestError` — the `code` property
  - Python: `SubmitError`/`RequestError` — the `code` attribute
  - C++: `submit_error_t`/`request_error_t` — the `.code()` method
  - Go: `*SubmitError`/`*RequestError` — the `Code()` method (interface)
  - Rust: `Err(SubmitError{..})`/`Err(RequestError{..})`, or at a
    multi-function-family boundary,
    `Err(ZlinkError::Submit(..))`/`Err(ZlinkError::Request(..))` — the
    `.code()` method

## Length And Range Boundary Policy
- Validation responsibility splits into two layers.
- For a type where a value object exists:
  - Perform canonical validation at value-object construction time.
  - Example: `RoutingId`, a typed enum wrapper, a bounded identifier
- For a type without a value object, or one that needs a call-context-
  dependent conversion:
  - Validate immediately before the native call.
  - Example: `Duration -> int millis`, offset/length slicing, output
    buffer sizing
- Re-validation immediately before the native call is required only in
  these cases:
  - a raw path exists that bypasses the value object
  - an additional conversion happens between value-object construction
    and the call
  - overflow/truncation can occur from a non-value-object composite
    input combination
- Passing a truncated value to native is forbidden.

Examples:
- `RoutingId` must not exceed `zlink_routing_id_t`'s `data[255]`
  contract.
- A `Duration -> int millis` conversion must not allow overflow.
- A path involving a fixed output buffer, such as topic, subscription,
  or metadata, must have a clear length and reallocation policy.

## Ownership Policy
- `Message` ownership must match the core contract.
- Because every binding calls the C API internally, every language,
  including GC languages, must correctly manage the ownership of the
  native message.
- Ownership paths:
  - send success: ownership moves to native. The binding must not
    access it afterward.
  - send failure: don't confuse a restorable path with a consumed path.
  - recv: the binding receives ownership of the message native created.
    The binding is responsible for releasing it.
  - constructed but not sent: if the binding constructed a message
    directly and never sent it, it must be explicitly closed/released.
    GC only collects the managed wrapper — it doesn't release the native
    memory — so a leak occurs otherwise.
- Callback delivery and direct receive must have the same payload
  shape.
- Frame validity after the callback must be clear from the contract.

## Naming Policy
- A method name reflects only language convention.
- Keep the concept name as identical as possible across bindings.
- The list below gives the canonical names by meaning.
- The actual binding method name allows only the following three
  variations.
  1. **Casing variation**: convert to camelCase/PascalCase/snake_case
     per language convention. Word composition does not change.
     - example: `connectPeer` → Go: `ConnectPeer`, Python:
       `connect_peer`, C++: `connect_peer`, Rust: `connect_peer`
  2. **A minimal suffix for a language without overloads**: in a
     language without overloading, like Go and Rust, a minimal suffix is
     allowed to distinguish a parameter variant of the same operation.
     This suffix distinguishes the operation — it is not parameter
     encoding.
     - example: `send` → Go: `Send`/`SendTo`, Rust: `send`/`send_to`
     - allowed suffix scope: only up to a minimal operation-distinguishing
       suffix at the `To` level. A suffix that spells out the parameter
       type or meaning is forbidden.
       - allowed: `SendTo`, `send_to`
       - forbidden: `SendWithRoutingId`, `send_routed`, `send_multipart`
     - suffix allowance applies only to a language with neither
       overloading nor keyword/optional parameters (Go, Rust).
     - a language that can distinguish by signature without a suffix
       does not use one.
       - overloading: Java, C#, C++
       - keyword/optional parameter: Python
       - optional/union type: Node/TypeScript
  3. **Per-language property/getter convention**: a value-reading
     accessor can use a property or getter form that fits language
     convention. However, the concept name must stay the same, and a new
     operation name must not be created.
     - example: canonical `getValue` → C++ `value()`, .NET `Value` or
       `GetValue()`, Java/Node `getValue()`
     - example: canonical `routingId`/`getRoutingId` → C++
       `routing_id()`, Java `routingId()`, Node `getRoutingId()`
- **No other word substitution, word omission, or word replacement is
  allowed.**
  - forbidden example: changing `setDispatchHandler` to
    `spotDispatchHandler` → word substitution
  - forbidden example: shortening `querySnapshot` to `snapshot` → word
    omission; if this is desired, the canonical name itself must be
    defined as `snapshot`
- Even when casing or the suffix differs, the role distinction and
  semantic contract must stay the same.
- example: `receiveSubscriptionEvent` → Python:
  `receive_subscription_event`, Go: `ReceiveSubscriptionEvent`
- Recommended canonical names:
  - `bind`, `connect`, `close`
  - `send`
  - `recv`
  - `publish`
  - `subscribe`
  - `receiveSubscriptionEvent`
  - `setSubscription`, `unsetSubscription`
  - `setPacketHandler`, `setDispatchHandler`, `setSendReadyHandler`

### Method Name Conciseness
- This rule applies strictly to the public API.
- An internal/private API is allowed to encode parameters when that
  improves readability.
  - Internal code may read better with an explicit name, without
    overloading.
  - example: `sendRouted(id, msg)` is allowed in an internal helper
- A method name expresses only the action.
- The presence, type, and count of parameters are not repeated in the
  name.
- Do not restate in the name what the signature already describes.
- When the operation itself differs (for example, `send` vs `publish`),
  the name must differ.
- When only the input differs (for example, whether a routing id is
  present), the name is not lengthened.

Anti-pattern vs. correct pattern:

| Anti-pattern | Correct pattern | Reason |
|---|---|---|
| `send(message)` | `send().message(message).submit()` | The start point takes only the send target; payload is a separate builder step |
| `sendWithRoutingId(id, msg)` | `send(id).message(msg).submit()` | The builder separates RoutingId and payload into steps |
| `sendMultipartMessages(parts)` | `send().message(p1).message(p2).submit()` | Multipart is expressed through repeated builder `.message(...)` calls |
| `publish(topic, message)` | `publish(topic).message(message).submit()` | Topic and payload are not mixed into one start point |
| `publishToTopic(topic, msg)` | `publish(topic).message(msg).submit()` | publish is already the topic-having operation; the builder separates payload into a step |
| `sendToChannel(channel, message)` | `sendToChannel(channel).message(message).submit()` | The channel target and payload are separated into builder steps |
| `requestToChannel(channel, parts, timeout)` | `requestToChannel(channel).message(p1).message(p2).timeout(timeout).submit()` | A channel request's payload and timeout are builder steps |
| `requestFrame(seq, parts)` | forbidden on the public surface | Request sequence and frame layout are runtime/internal helper detail |
| `dealer.reply(token, parts)` | `received.reply().message(...).submit()`, or a router/SPOT reply | DEALER cannot designate a specific peer routing id, so an arbitrary-token reply doesn't conceptually fit |
| `recvWithTimeout(timeout)` | `recv(timeout)` | The signature is enough |
| `setLingerTimeoutMilliseconds(ms)` | `setLinger(duration)` | The type conveys the unit |

The send/request/reply/publish/Actor surface exposes only the builder
start point, per the `Operation Builder Policy`, and every variation
axis — payload, flags, timeout, callback — is expressed as a builder
step. A start-point name carries only the action, and does not repeat
the presence, type, or count of parameters.

For a non-builder public surface (for example, snapshot, lookup,
getter/setter) where the parameter combination differs, use each
language's own disambiguation mechanism instead of lengthening the name.

- Java/C#/C++: overloading
  - one name; the signature distinguishes
- Go: variadic arguments/functional option/a separate method
  - because there's no overloading, allow a minimal suffix only when the
    operation meaning differs
  - do not put the parameter directly into the name
- Python: keyword argument/optional parameter
  - one name; the keyword distinguishes
- Node/TypeScript: optional parameter/union type
  - one name; the type distinguishes
- Rust: trait bound/`Option<T>`/newtype
  - because there's no overloading, distinguish with `impl Into<T>`,
    `Option<T>`, or a strong newtype
  - allow a minimal suffix only when the operation meaning differs
  - do not put the parameter directly into the name

Per-language summary:

| Language | Disambiguation method | Parameter encoding in the name |
|---|---|---|
| Java | overloading | forbidden |
| C# | overloading | forbidden |
| C++ | overloading + strong type | forbidden |
| Go | separate method/functional option | forbidden; only an operation-distinguishing suffix is allowed |
| Python | keyword/optional | forbidden |
| Node/TS | optional/union | forbidden |
| Rust | trait bound/Option/newtype | forbidden; only an operation-distinguishing suffix is allowed |

## Compatibility Policy
- A consistent public surface can be prioritized over compatibility.
- Remove a deprecated compatibility layer as soon as possible.
- Do not keep a bypass surface for the same capability publicly
  alongside the canonical path.
- Flag type policy:
  - Whether and how public flags are exposed follows the `Flags Policy`
    section above.
  - .NET's `SendFlags`/`RecvFlags` public surface is the canonical
    contract.
  - Do not add a legacy flag type or duplicate flag path that isn't in
    the per-language spec.

## Cross-Language Alignment

### Shared Behavior Contract
- The blocking send/receive family delivers a per-language error path
  on failure (an exception for exception languages, a returned error
  for return-based languages)
- Non-blocking receive delivers "no data" through the same error path
  too (distinguished by result code). No separate `try*` API is
  provided.
- Non-blocking send has an explicit outcome (a submit result code)
- multipart-only
- a typed option surface

### Per-Language Return Style
- C API
  - the raw contract and a per-function typed result enum
  - a multipart-only baseline surface
  - a blocking API plus an explicit non-blocking entry (a `flags`
    parameter)
- C++
  - RAII and typed wrappers
  - a multipart-only baseline surface
  - failure is `throw zlink_error_t` (including the `SubmitResult` code)
- .NET
  - a typed option surface plus `ZlinkException`
  - a multipart-only baseline surface
  - failure is `throw ZlinkException` (including `Code`)
- Java
  - domain objects plus `ZlinkException`
  - a multipart-only baseline surface
  - failure is `throw ZlinkException` (including `getCode()`)
- Go
  - `(T, error)` plus strong types and an explicit error check
  - a multipart-only baseline surface
  - every failure returns `error` (including the `SubmitResult` code)
- Rust
  - `Result<T, E>` plus a strong newtype and ownership
  - a multipart-only baseline surface
  - a single function family uses a concrete error such as
    `BindError`/`SubmitError`; multiple function families use
    `ZlinkError`
- Node/Python
  - follows language convention, but the semantic contract is the same
  - a multipart-only baseline surface
  - every failure is `throw`/`raise` (including the `SubmitResult` code)

The surface can differ per language, but the semantic contract must be
the same.

### Cross-Language Capability Table (Target)
This table is a target role table organized around `.NET`. If an
already-implemented binding's current public surface differs from this
table, interpret that item as a goal for structural alignment or
breaking cleanup work. However, an `Internal-only` item is not exposed
in the public API, samples, guides, or spec signatures even in its
target state.

| Area | C API | C++ | .NET | Java | Go | Rust | Node | Python |
|---|---|---|---|---|---|---|---|---|
| Multipart-only public surface | Required | Required | Required | Required | Required | Required | Required | Required |
| Blocking API named directly | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| Non-blocking receive uses flags + empty result | C raw `DONTWAIT` | Required | Required | Required | Required | Required | Required | Required |
| Non-blocking send explicit outcome | Core enum/result | Required | Required | Required | Required | Required | Required | Required |
| Public flags surface | Raw C flags | `int flags` | `SendFlags` / `RecvFlags` | `SendFlags` overload | `flags SendFlags` | `SendFlags` via `.flags(...)` builder step | `flags?: SendFlags` | keyword `flags` |
| Typed option surface | N/A raw C options | Required | Required | Required | Required | Required | Required | Required |
| Socket TLS helpers | `zlink_set_tls_*` | Required | Required | Required | Required | Required | Required | Required |
| Service TLS helpers | `zlink_set_tls_*` on service handles | Required | Required | Required | Required | Required | Required | Required |
| Socket Capability Matrix compliance | Based on Core | Required | Required | Required | Required | Required | Required | Required |
| `onReceive` callback | STREAM raw fn ptr | Internal-only | Internal-only | Internal-only | Internal-only | Internal-only | Internal-only | Internal-only |
| `setPacketHandler` callback registration | STREAM packet fn ptr | Required | Required | Required | Required | Required | Required | Required |
| `setDispatchHandler` callback registration | SPOT raw fn ptr | Required once implemented | Required once implemented | Required once implemented | Required once implemented | Required once implemented | Required once implemented | Required once implemented |
| `recvActorLifecycle` | SPOT lifecycle queue | Required | Required | Required | Required | Required | Required | Required |
| `setSendReadyHandler` callback registration | Raw fn ptr | Required | Required | Required | Required | Required | Required | Required |
| StreamSocket `connect` blocked | N/A | Required | Required | Required | Required | Required | Required | Required |
| StreamSocket `disconnectRid` blocked | N/A | Required | Required | Required | Required | Required | Required | Required |
| Public `detachStream` not exposed | N/A | Required | Required | Required | Required | Required | Required | Required |
| Poller result type name | N/A | `poll_event_t` | `PollEvent` | `PollEvent` | `PollEvent` | `PollEvent` | `PollEvent` | `PollEvent` |
| Monitor typed event surface | Raw struct | Required | Required | Required | Required | Required | Required | Required |

## Test Policy
The purpose of binding tests is not to match a test count across
languages. The purpose is to confirm that each binding guarantees, at
the same level and without omission, the contract that matches its own
public surface.

The test count can vary by per-language API surface, runtime ownership
model, and packaging approach. So the test count is only a supporting
signal for judgment — the real standard is whether the verification
layers below and the Test Matrix's semantic contract are satisfied. If a
particular binding's test count looks unusually high or low, first check
whether a contract is missing, or whether a redundant test — such as one
that re-verifies core correctness — has crept in, rather than trying to
match the count itself.

A zlink binding is not a thin wrapper that simply calls a native
function. Each language binding also provides a public facade, helper
objects, domain objects, typed options, callback delivery, ownership
management, a native loader, a package boundary, and hot-path
optimization. So a simple round-trip test is not enough either — the
additional meaning a public helper provides, and its optimization
invariants, must also be verified as part of the binding contract.

Tests are classified into the layers below.

- `Required`: a test every binding must have.
- `Conditional`: a test required only for a binding that provides the
  matching public API or distribution unit.
- `Language-specific`: a test that verifies a risk specific to a
  runtime — lifetime, exceptions, GC, borrow, cgo, native loader. This
  is not forced to be duplicated in another language.
- `Sample smoke`: a test that confirms a user-facing pattern runs
  through the public API. This does not expand into a large scenario set
  that re-verifies core correctness.
- `Out of scope`: core's own messaging correctness, a full re-
  verification of the transport matrix, one-off migration verification,
  and review items that can't be automated. These items are not kept as
  permanent binding tests. However, if a path involves a binding helper,
  facade, or optimization invariant, it stays a binding test even if it
  looks like it overlaps with core functionality.

The shared principles are:

- A public surface test fixes the canonical public API.
- A contract test verifies type conversion, error mapping, and handle
  lifecycle at the binding/native boundary.
- A behavior test verifies that the binding public API correctly relays
  the core contract.
- A helper/facade test verifies the semantic contract of the extra
  language-friendly functionality a binding provides.
- An ownership test must cover the send-success, send-failure, receive,
  callback, and multipart paths.
- An optimization guard test verifies the hot path hasn't regressed into
  a slow path the policy forbids.
- A path where callback mode and direct mode are not allowed together
  verifies the conflict rule.
- An option test verifies the typed option surface together with
  blocking incorrect role access.
- Performance regression verification is owned by the separate Perf
  Policy. A functional test must not replace a perf benchmark, and a
  perf benchmark must not replace a public contract test.

The test satisfaction criteria are:

- Each binding must verify every `Required` item in the Test Matrix for
  the public API it provides.
- If it provides a particular public API, extension package, or sample
  suite, it must also verify the matching `Conditional` item.
- A risk that arises from the language runtime — ownership, lifetime,
  loader, callback, GC, borrow, cgo — is verified with a
  `Language-specific` test.
- Do not add a test for a public API the binding doesn't provide, just
  to match a count.
- A test that re-verifies core correctness is removed from the binding
  tests or moved to a core test, unless it directly relates to a
  binding helper, facade, package boundary, native loader, or
  optimization invariant.
- If multiple tests repeatedly verify the same contract, merge them into
  one deep test; if one test hides multiple distinct contracts, split it
  so each Matrix item is visible.

Required test rules on a policy change:

- a public surface change: accompanied by a public surface test
- a contract change: accompanied by a contract test
- a blocking/non-blocking contract change: accompanied by a behavior
  test
- an ownership/receive shape change: accompanied by a callback
  regression or ownership test
- an option surface change: accompanied by a typed option surface test
  and a negative role test
- a codec extension change: accompanied by that codec extension's test
- a helper/facade change: accompanied by a helper/facade contract test
- a hot-path implementation change: accompanied by an optimization
  guard test or a perf regression gate

If existing code has a test outside the Test Matrix, clean it up by the
following criteria.

- If it re-verifies core functionality, move it to a core test or
  delete it.
- If it's migration verification, mark it as a temporary test to delete
  once migration is complete.
- If it confirms a user-facing pattern, move it to sample smoke.
- If it verifies a binding helper, facade, package boundary, native
  loader, or optimization invariant, keep it, classified under the
  appropriate Test Matrix category.
- If it verifies a specific language runtime risk, keep it as a
  Language-specific test, and make the reason clear from the test name
  or file name.

### Test Run Script Policy
- Each binding must provide a script that can run the entire test suite
  at once.
- The run script must be located in the `bindings/<language>/tests/`
  directory.
- The script must be repeatable and must summarize success/failure.
- Recommended forms:
  - `tests/run_tests.sh`
  - `tests/run_tests.ps1`
  - a language-specific test runner entry

### Bug Discovery Policy
- When a bug is found while writing/running a test or perf benchmark,
  follow this procedure.
- A binding library bug:
  - Fix it directly in that binding.
  - Add a regression test together with the fix.
- A core library bug:
  - Do not fix the core bug directly from the binding.
  - Write a bug report in the `bindings/<language>/bug/` directory.
  - The report must include at least the following:
    - reproduction conditions (socket type, pattern, message size,
      transport, and so on)
    - expected behavior
    - actual behavior
    - reproduction code or a test reference
  - If a workaround is needed on the binding side, explicitly mark it as
    a workaround and reference the bug report.

## Test Matrix
- This section lists the minimum test items each binding must have.
- Even though the surface differs per binding, every semantic contract
  below must be verified.
- `Surface Tests`, `Contract Tests`, `Behavior Tests`, `Failure Contract
  Tests`, `Helper/Facade Tests`, `Optimization Guard Tests`, `Boundary
  Validation Tests`, `Option Tests`, `Ownership Tests` are baseline
  `Required` items for every binding.
- `Callback Tests`, `Monitor Tests`, `Poller Tests`, `Service Tests`,
  `Codec Tests`, `Sample Smoke Tests` are `Conditional` items for a
  binding that provides the matching public API, extension package, or
  sample suite.
- `Language Runtime Tests` are `Language-specific` items for a binding
  where runtime characteristics create a risk.

### Required: Surface Tests
- a canonical public API surface test
- confirming socket-type role separation
- confirming a typed option surface exists
- confirming the shared socket TLS helper exists
- confirming the service TLS helper exists
- confirming a raw option bag is not exposed
- confirming a monitor canonical surface exists
  - `recv()`

### Required: Contract Tests
- verifying FFI/native call mapping
  - confirming a binding public API call maps to the correct C API
    function
  - confirming parameter passing and return-value conversion are
    correct
- verifying type conversion at the managed ↔ native boundary
  - confirming the language-type-to-C-type conversion is correct
  - confirming the C-type-to-language-type conversion is correct
- verifying resource lifecycle
  - confirming context/socket native handle construction and release
    work without leaking
  - confirming native resources are cleaned up on exception/error paths
    too

### Required: Behavior Tests
- Verify the binding layer correctly relays the core contract.
- The goal is confirming the correctness of the binding path, not
  re-verifying core messaging functionality.
- Blocking paths:
  - `send` → successfully relays to core send
  - `recv` → successfully relays to core recv
  - `publish` → successfully relays to core publish
  - `subscribe` → successfully relays to core subscribe
  - routed `send` → successfully relays including the routing id
- Non-blocking paths:
  - `recv` non-blocking → returns empty when there's no data
  - `subscribe` non-blocking → returns empty when there's no data
  - `receiveSubscriptionEvent` non-blocking → returns empty when there's
    no data
  - confirms an exception or error path on `send` failure
  - confirms an exception or error path on `publish` failure

### Required: Helper/Facade Tests
- When a public helper or facade provides meaning beyond a simple
  native call, directly verify that meaning.
- Verify the invariants of binding-provided types such as `Message`,
  `Received`, a multipart collection, a routing id value/codec, a typed
  option facade, a domain object, a request/reply helper, and a topology
  snapshot value object.
- Confirm a helper does not leak native detail to the user.
- Confirm a helper keeps success/failure, an empty payload, one empty
  message, and multipart boundaries distinct.
- Confirm a helper does not require the user to perform internal
  sequencing that isn't in the public API.
- Confirm a convenience API does not create a meaning different from
  the canonical API.

### Required: Optimization Guard Tests
- Verify the hot path continues to satisfy the High-Performance Binding
  Policy.
- Confirm the internal send/recv/request/reply/publish/subscribe path
  uses the `*_part` substrate.
- Confirm an aggregate native function call, hidden double
  materialization, an unnecessary eager copy, or a per-call closure/
  boxing/allocation hasn't crept back in.
- Confirm a hidden blocking wait, sleep, busy wait, or thread join
  hasn't appeared on the callback, dispatch, poller, or request-
  completion path.
- This verification does not always need to be a micro benchmark. When
  it can be automated reliably, use whichever of a source-level/static
  check, a public API allocation check, a stress smoke test, or a perf
  gate costs the least.
- A perf benchmark owns numeric regression, while an optimization guard
  test owns keeping a forbidden structure out of the code.

### Required: Failure Contract Tests
- confirm a blocking `send` failure is delivered to the caller through
  an exception or the per-language error path
- confirm a blocking `publish` failure is delivered to the caller
  through an exception or the per-language error path
- confirm the `send` backpressure exception
- confirm the `send` not-ready exception
- confirm the `publish` backpressure or not-ready exception
- confirm an error other than native `NO_DATA` is not ignored
- confirm an error is delivered per the native contract when callback
  mode and direct recv conflict
- confirm a state where direct recv is impossible is not hidden as
  empty/null
- confirm only native `NO_DATA` is treated as an empty/non-success
  result

### Required: Boundary Validation Tests
- `RoutingId`'s maximum length boundary (255 bytes OK)
- `RoutingId` over the length returns an immediate error (256+ bytes →
  exception)
- the `Duration -> int millis` overflow boundary
- offset/length bounds validation
- non-nullable argument validation
- out-of-enum-range value validation
- `channel_name` over 255 bytes returns an immediate error (fixed-size
  `char[256]`)
- `endpoint` over 255 bytes returns an immediate error (fixed-size
  `char[256]`)
- an embedded null character in topic/filter returns an immediate
  error

### Required: Option Tests
- common option typed getter/setter
- per-socket-type typed option getter/setter
- blocking an option-role access on the wrong socket type
- confirming an enum/boolean surface is provided instead of a raw
  integer

### Required: Ownership Tests
- the ownership-transfer contract on send success (moves to native; the
  binding must not access it afterward)
- the restore-or-caller-keeps-ownership contract on send failure
- explicit close/release of a constructed-but-unsent message (native
  memory leaks without close)
- the ownership contract of a recv result (the binding receives it and
  is responsible for releasing it)
- the frame-validity contract after a callback
- whether the multipart receive shape and the callback delivery shape
  match

### Conditional: Callback Tests
- If there's a public callback API, verify callback delivery.
- Verify the ownership of the message or multipart payload the callback
  receives.
- Confirm a per-language failure — a callback exception, panic,
  rejected promise, delegate exception — is delivered through the
  documented error path.
- Verify a callback delegate/function/object's lifetime doesn't outlive
  the native callback in the wrong direction and create a
  use-after-free.
- Confirm a forbidden blocking wait or hidden thread join doesn't occur
  inside the callback.

### Conditional: Monitor Tests
- the successful blocking monitor `recv` path
- the non-blocking monitor recv empty path
- whether the monitor callback/state change matches data-plane
  readiness

### Conditional: Poller Tests
- Confirm raw socket readiness or fd readiness is delivered through the
  public poller API.
- Confirm the poller doesn't silently accept a service-specific handle
  it doesn't support.
- Verify a readiness event value does not replace the data-plane
  contract.

### Conditional: Service Tests
- A binding that provides the spot/actor public API verifies that
  service's lifecycle through a minimal path.
- Confirm a lifecycle constraint such as close/connect/unbind is
  delivered through the public API per the native contract.
- For spot publish/subscribe, spot request/reply, and SPOT status/
  snapshot, perform a round-trip or snapshot verification wherever a
  public surface exists.
- A service test's goal is verifying the service-layer binding
  contract. It does not re-run the entire core service matrix in every
  language.

### Conditional: Codec Tests
- A binding that provides a codec extension package verifies a
  per-codec payload round trip.
- Confirm the core binding package doesn't pull in a codec dependency as
  required.
- A language with a serializer-selection rule verifies the default
  serializer and its error path.

### Conditional: Sample Smoke Tests
- A binding that provides a sample suite provides a run smoke test for
  the canonical sample set.
- Sample smoke is the minimal verification that confirms the public API
  is usable.
- Sample smoke does not replace the core transport matrix, stress
  testing, or perf measurement.

### Language-specific: Runtime Tests
- .NET: verifies `IDisposable`, `SafeHandle`, delegate lifetime,
  `GCHandle`, the native library loader, and `ZlinkException` mapping.
- Java: verifies `AutoCloseable`, JNI object lifetime, the checked/
  unchecked exception policy, and the classloader/native loader
  boundary.
- Go: verifies the cgo pointer rule, an explicit close that doesn't
  depend on a finalizer, and `(T, error)` mapping.
- Rust: verifies ownership move, borrow lifetime, `Drop`, whether
  `Send`/`Sync` is exposed, and concrete error type mapping.
- Python: verifies the buffer protocol, reference counting, the context
  manager, and exception mapping.
- Node: verifies native addon lifetime, `Buffer` ownership, the async
  callback error path, and the package export boundary.
- C++: verifies RAII, move-only message ownership, the exception type,
  and the installed header boundary.
- C: verifies the raw ABI, errno/result codes, and caller-provided
  message lifecycle.

### Note: Performance And Sample Verification
- Performance regression verification is owned by the Perf Policy
  (`doc/perf/`). It is not duplicated in the Test Matrix.
- A sample/helper's compliance with the canonical API, avoiding
  ignoring a send failure, and avoiding a legacy-surface bypass are
  verified in the Review Checklist. These are not automated test items.

## Sample Policy
- Sample-authoring rules use
  [`doc/spec/sample/SAMPLE_POLICY.md`](https://kairos-code-dev.github.io/zlink/en/spec/sample/SAMPLE_POLICY/)
  as the single baseline document.
- This document covers `core/samples/` and `bindings/*/samples/`
  together.
- Adding, changing, or reviewing a binding sample is judged against
  that document.

## Perf Policy

Perf code is not a demo — it's code for measuring and improving the
binding library's performance. Perf's primary purpose is to reveal the
binding layer's cost, identify bottlenecks and regressions, and measure
before/after differences from improvement work.

**The single baseline for perf policy is the `doc/perf/` policy
documents.** Every detailed specification — CLI options, defaults,
output format, RESULT line format, the pattern/transport matrix, phase
rules, result storage, failure handling, environment variables — follows
the documents below. This section does not redefine them.

- [`doc/perf/PERF_POLICY.md`](../../../doc/perf/PERF_POLICY.md) — the
  shared perf policy (shared principles, directory structure, RESULT
  format, result storage, output format, failure handling, environment
  variables, refactoring principles, per-language scope)
- [`doc/perf/PERF_SINGLE_TEST_POLICY.md`](../../../doc/perf/PERF_SINGLE_TEST_POLICY.md) — the single-suite policy
- [`doc/perf/PERF_MULTI_TEST_POLICY.md`](../../../doc/perf/PERF_MULTI_TEST_POLICY.md) — the multi-suite policy

### Binding Perf Principles

- Perf code follows the `doc/perf` policy.
- It's based on the patterns and scenarios `core/perf` provides.
- It keeps a scenario comparable to core perf, while writing it to fit
  each language's style.
- The measurement anchor point, phase meaning, metric set, and RESULT
  line meaning do not change.
- The perf policy is `Required` for a binding that officially provides a
  performance-measurement surface. A binding that doesn't yet provide
  perf code treats it as `Target`.

### Binding API Spec Documents

Each binding's API surface is documented in the file below.
The perf policy is managed across every language in a shared way, in
[`doc/perf/PERF_POLICY.md`](../../../doc/perf/PERF_POLICY.md).

| Binding | API Spec |
|--------|----------|
| C | [`c/README.md`](c/README.en.md) |
| C++ | [`cpp/README.md`](cpp/README.en.md) |
| Java | [`java/README.md`](java/README.en.md) |
| .NET | [`dotnet/README.md`](dotnet/README.en.md) |
| Node.js | [`node/README.md`](node/README.en.md) |
| Python | [`python/README.md`](python/README.en.md) |
| Go | [`go/README.md`](go/README.en.md) |
| Rust | [`rust/README.md`](rust/README.en.md) |

### Perf Review Checklist

- Does this perf measure the binding library's cost?
- Can the core send/recv/callback path be read directly in the perf
  file body?
- Is each pattern split into its own file?
- Is it aligned with `core/perf` patterns?
- Does it follow the `doc/perf` policy?

## Script Location Policy
- A run script lives in the same directory as its target.
- It goes in each subdirectory, not the binding root.

| Purpose | Location | Example script |
|------|------|---------------|
| Tests | `bindings/<language>/tests/` | `run_tests.sh` |
| Samples | `bindings/<language>/samples/` | `run_samples.sh` |
| Perf | `bindings/<language>/perf/` | `run_benchmarks.sh`, `run_benchmarks_multi.sh` |

- When Windows support is needed, also provide a `.ps1`.
- Do not put a wrapper such as `run_samples.sh` at the binding root
  (`bindings/<language>/`). A wrapper at that location duplicates
  `samples/run_samples.sh` and creates confusion about which one is
  authoritative.
- If an orchestration script is needed to run tests+samples+perf
  together for CI or full verification, it can be placed under a name
  such as `bindings/<language>/run_all.sh`. This script is an entrypoint
  that calls the individual `tests/run_tests.sh`, `samples/run_samples.sh`,
  and so on — it does not replace the individual scripts.

## Review Checklist
- Is the public API multipart-only?
- Are blocking/non-blocking not split into separate names?
- Is there no remaining public flag type or duplicate flag path that
  isn't in the per-language `Flags Policy`?
- Does no raw option bag remain public?
- Have option values been promoted to enum/boolean/value objects?
- Are per-type roles properly closed off?
- Is a blocking send failure always delivered to the caller through an
  exception or error path?
- Does a `send` failure deliver every error, including backpressure/
  not-ready, as an exception?
- Does the binding pre-validate truncation/overflow?
- Does the binding avoid arbitrarily inferring a native state error?
- Are the public surface test and behavior test present together?
- Is the division of responsibility between value-object validation and
  pre-call validation explainable?
- Has a legacy flag type not in the per-language `Flags Policy` been
  removed from the public contract?
- Does sample code use only the canonical API?
- Does the helper avoid ignoring a blocking send failure?
- Does the helper avoid bypassing through a deprecated/legacy surface?

## POSD-Based Implementation Completeness Policy
- This section defines the POSD-based procedure applied when completing
  or refactoring a binding implementation.
- A binding is completed against structural correctness, not a feature
  checklist.
- The completion criteria are the Socket Capability Matrix, Callback API
  Policy, Option Policy, Test Matrix, and Sample Policy.
- Refactoring is reducing system complexity, not moving code around.

### Completion Order
- A binding implementation follows the order below.
- Each step depends on the previous step's result.
- Do not skip a step and move to the next.

#### Step 1: Align The Capability Matrix
- Review each socket type's public API against the Socket Capability
  Matrix.
- Add an API that should exist but doesn't.
- Remove, or move to internal, an API that's exposed but shouldn't be.
- Verification: the surface test must match the matrix.
- Representative violation examples:
  - `connect()` exposed on StreamSocket → remove
  - `disconnectRid()` exposed on StreamSocket → remove
  - `detachStream()` exposed on StreamSocket → remove
  - `setSendReadyHandler` missing on Node → add
  - publish/subscribe exposed on the wrong socket → remove

#### Step 2: Normalize Names
- Align to the canonical name per the Naming Policy and Callback API
  Policy.
- Unify an API that differs only in name but means the same thing under
  the canonical name.
- Remove a deprecated alias.
- Verification: confirm the canonical name exists in the surface test.
- Representative violation examples:
  - a public `recvHandler`/`onReceive` → remove, or move to the
    internal raw STREAM bridge
  - `spotDispatchHandler` → `setDispatchHandler`
  - `on_topic_message` → `subscribe`

#### Step 3: Deep Module Structure
- Secure depth for public types, per the POSD deep-module principle.
- Confirm each public type isn't a simple pass-through — that it
  encapsulates validation, ownership, or shape rules internally.
- Criteria for identifying a shallow wrapper:
  - Does it only wrap a native function 1:1 without adding new meaning?
  - Can the caller use it only by knowing the native contract
    (sequence, size, encoding)?
  - Is the same rule redundantly implemented across multiple socket
    types?
- Once a shallow wrapper is found:
  - Move validation inside a value object or facade.
  - Gather the duplicated rule into one module.
  - Remove, or merge into internal, a public type that only does
    pass-through.
- Representative violation examples:
  - RoutingId length validation duplicated per socket type → gather
    into one RoutingId value object
  - a monitor event as raw int → promote to a typed event surface
  - an option value as raw int → promote to enum/boolean/Duration

#### Step 4: Eliminate Change Amplification
- Find where the same rule is scattered across multiple places and
  gather it into one module.
- Criteria for identifying it:
  - Does changing one policy require fixing 2 or more files?
  - Does adding a new socket type require modifying existing code in N
    places?
- Representative violation examples:
  - the send-failure contract rule implemented separately per socket
    type
  - blocking/non-blocking branching implemented separately per socket
    type
  - option validation implemented separately per option setter

#### Step 5: Strengthen Information Hiding
- Find where the public API exposes native detail and hide it behind a
  facade.
- Criteria for identifying it:
  - Does the user need to know errno, flag constants, or a native
    struct size?
  - Does the user need to remember internal sequencing (call order)?
  - Is a native handle, raw pointer, or raw buffer exposed in the public
    API?
- Representative violation examples:
  - a raw `setSockOptRaw`/`setOption(int, byte[])` is public
  - a raw int mask is exposed as-is in a monitor event
  - a legacy flag type not in the per-language `Flags Policy` remains a
    public type

#### Step 6: Complete The Test Matrix
- Write or strengthen the Test Matrix's `Required` category in every
  binding.
- A binding that provides the matching public API, extension package, or
  sample suite also writes or strengthens the matching `Conditional`
  category.
- A binding with a language-runtime lifetime, exception, or native-
  loader risk also writes or strengthens the matching
  `Language-specific` category.
- Completion criteria:
  - a Surface test verifies the Socket Capability Matrix
  - a Contract test verifies FFI mapping and lifecycle
  - a Behavior test verifies the blocking/non-blocking paths
  - a Helper/Facade test verifies the semantic contract of a
    binding-provided helper
  - an Optimization Guard test verifies the hot-path optimization
    invariant
  - a Failure Contract test verifies the send/receive error contract
  - a Boundary test verifies value boundaries
  - an Option test verifies the typed surface
  - an Ownership test verifies send/recv ownership
  - where the matching public API exists, a Callback, Monitor, Poller,
    Service, or Codec test verifies the public contract
  - where a sample suite exists, a Sample Smoke test verifies running
    the canonical API

#### Step 7: Align Samples
- Complete samples against the Canonical Sample Set.
- Confirm each sample uses only the canonical API.
- If a name or API changed in steps 1–5, update the samples too.

### Refactoring Judgment Criteria
- If the answer to the following question is "yes," that's a point that
  needs refactoring.
  - Would the user lose nothing if this public type were removed? →
    shallow wrapper
  - Would fixing this rule require touching 3 or more files? → change
    amplification
  - Does the user need to know another API's internal behavior to use
    this one? → information leak
  - Is the same capability exposed under 2 or more names? → duplicate
    surface
  - Does correct behavior require the user to remember a call order? →
    temporal decomposition dependency

### Refactoring Exit Conditions
- Refactoring repeats until every condition below is satisfied.
- If even one remains, it isn't done.
- The judgment is made from a POSD perspective.
- The exit-condition scope is limited to what that binding has decided
  to implement.
  - a `Required` item: applies to every binding
  - a `Conditional` item: applies to a binding that provides the
    matching public API, extension package, or sample suite
  - a `Language-specific` item: applies to a binding with that runtime
    risk
  - a `Recommended` item (for example, samples): applies to a publicly
    distributed binding

1. **Full Capability Matrix Alignment**
   - Every `Y` item in the Socket Capability Matrix exists in the
     public API.
   - Every `—` item in the Socket Capability Matrix is not exposed in
     the public API.
   - The Capability Matrix of any service-layer component that binding
     implements is aligned the same way. If a binding doesn't implement
     it, exclude it from the exit condition.
   - The surface test verifies this and passes.

2. **Name Normalization Complete**
   - Every public API uses the Naming Policy's canonical name.
   - No deprecated alias remains.
   - The Callback API Policy's canonical names (`setPacketHandler`,
     `setDispatchHandler`, `setSendReadyHandler`) exist matching their
     roles.

3. **Shallow Wrappers Removed**
   - No public type merely wraps a native function 1:1.
   - Every public type encapsulates at least one of validation,
     ownership, or shape rules.
   - `RecvPart`, `RecvRoutedPart`, `SubscribePart`, or a per-language
     equivalent name, does not exist in the public API. Part-by-part
     receive exists only as a runtime/internal substrate.
   - A helper that exposes the protocol envelope as-is, such as
     `requestFrame(...)`, does not exist on the public surface.
   - A reply helper that doesn't fit DEALER's send capability, such as
     `dealer.reply(requestToken, parts)`, does not exist on the public
     surface.

4. **Change Amplification Resolved**
   - The same rule is not redundantly implemented in 2 or more modules.
   - Only 1 file needs to change on a policy change.

5. **Information Hiding Secured**
   - The public API does not expose a raw option bag, a legacy/raw flag
     outside the policy, a raw native struct, or a raw errno.
   - A user can use the API correctly without knowing internal
     sequencing.

6. **Test Matrix Complete**
   - Every `Required` test exists and passes.
   - Every `Conditional` test within that binding's scope exists and
     passes.
   - Every `Language-specific` test needed for that runtime risk exists
     and passes.

7. **Sample Alignment Complete**
   - Every sample in the Canonical Sample Set exists.
   - This includes a sample for any service-layer component that
     binding implements.
   - A sample for an unimplemented `Target` component is excluded.
   - Every sample uses only the canonical API.
   - No sample uses a deprecated/legacy path.

8. **Dead Code Removal Complete**
   - Every unnecessary piece of code produced during refactoring has
     been removed.
   - No deprecated alias, legacy wrapper, or unused import/using/
     require remains.
   - Implementation code for an API marked `—` in the Capability Matrix
     doesn't needlessly remain in internal either.
   - No function/method/type under an old name replaced by name
     normalization remains.
   - No uncalled private/internal helper remains.
   - No unreferenced constant, enum value, or type alias remains.
   - No commented-out code block (`// removed`, `// deprecated`,
     `// remove later`) remains.
   - No empty file, empty class, or empty module remains.
   - Dead code is not kept "in case it's needed later." Restore it from
     git history if needed.

### Refactoring Iteration Rule
- After running through steps 1–7 once, recheck the exit conditions.
- Because an earlier step's change can affect a later step, if even one
  exit condition is unmet, redo starting from that step.
- Repeat until all 8 exit conditions are satisfied.
- The completion criterion is not "no more spots visibly need fixing" —
  it's "all 8 exit conditions pass."

### Refactoring Prohibitions
- Do not change the semantic contract in the name of structural
  improvement.
- An internal refactor must not change the public API's signature.
  - If the signature must change, that's an API change, not a
    refactor.
- Do not compromise correctness in the name of a performance
  improvement.
- Do not pre-build an abstraction "in case it's needed later."
- Do not pull code used only once out into a utility/helper.

## Implementation Review Checklist
- This section is a review checklist for confirming the public API
  policy has been reflected in the implementation.
- The items below are not a proposal for new public API. They're the
  standard for confirming the implementation, tests, and samples follow
  the already-defined contract and boundary rules.
- Use these items as the base checklist for per-binding review and
  refactoring work.

### Public vs. Internal Boundary Follow-Up
- Java:
  - An internal-natured type that remains in a public package
    (`SocketCore`, `MessagePlane`, a request/reply support helper, and
    so on) must be moved to an internal or implementation package.
  - If JPMS is used, clean it up so only documented public packages are
    exported.
- .NET:
  - `InternalsVisibleTo` must be restricted to test-support scope only.
  - Assembly visibility must be closed back up so the perf project
    doesn't access the internal surface.
- C:
  - Once the helper substrate and the public C binding header are
    actually separated, the `core/include/zlink.h`-centered explanation
    must be reorganized around the public C binding header.
  - The boundary between the installed public header and the private
    substrate header must be reflected in both the documentation and
    packaging.

### Value Validation Follow-Up
- `RoutingId`
  - length-limit validation at value-object construction
  - re-validation immediately before the native call, if a raw path
    remains
- `Duration`-based options
  - `int millis` conversion overflow validation
  - explicitly stating the negative-value allowed/disallowed contract
- topic/filter/string identifiers
  - check the reallocation policy on a fixed-size output buffer path
  - check that the entire string is processed without truncation
- offset/length-based byte APIs
  - unify bounds validation
- a raw integer option without an enum wrapper
  - investigate enum or boolean promotion candidates

### Public Surface Follow-Up
- a legacy flag type
  - reconfirm removal of a public flag type or duplicate flag path not
    in the per-language `Flags Policy`
  - decide whether to move it to internal, if needed
- the monitor plane
  - confirm the `recv()` canonical surface is preserved
- the callback API
  - reconfirm the callback payload shape matches the direct-receive
    shape
- a single-message convenience method
  - check for a remaining public receive/subscribe convenience overload

### Option Surface Follow-Up
- investigate any remaining raw option bag
- investigate any option-role leak across socket types
- list an option value that still stays `int`
- review whether a context option should also get a typed facade under
  the same standard

### Error Contract Follow-Up
- investigate a path where a binding validation exception and a native
  exception are mixed
- investigate a path where the binding arbitrarily interprets errno
- investigate code that wrongly hides an error other than native
  `NO_DATA` behind an empty/bool path
- investigate a helper/sample that ignores a blocking send failure

### Performance Follow-Up
- investigate a hidden copy on the hot-path send/recv path
- investigate unnecessary collection/array allocation while
  constructing `Message`, `Received`, `TopicMessage`
- investigate the cost gap between the callback path and the direct
  path
- investigate the encoding/decoding cost of string/topic/routing-id
  conversion
- investigate whether a sample or helper exposes a slow alternative
  path as the default usage pattern

### POSD Follow-Up
- investigate a public type that provides only a shallow wrapper
- investigate a change-amplification point where one rule is scattered
  across multiple modules
- investigate a temporal API that requires the user to know internal
  sequencing
- investigate a raw/native concept leak point that could be hidden
  behind a facade

### Ownership And Callback Follow-Up
- check that the send-failure restore path and consume path match the
  documentation
- re-verify the frame-validity contract after a callback
- check that an error is delivered per the native contract when
  callback mode and direct recv conflict

### Test Follow-Up
- confirm a public surface test exists for every public surface change
- add value-boundary verification tests
  - example: `RoutingId`'s maximum length
  - example: `Duration` overflow
- strengthen option negative-role tests
- confirm ownership/callback regression tests are kept

## Binding Requirements

| Binding | Language version | Runtime/framework | Build tool |
|---------|-----------|-------------------|---------|
| C++ | C++20 | — | CMake 3.10+ |
| .NET | C# 12 | .NET 8.0 | MSBuild |
| Java | Java 22 | JDK 22 | Gradle 8.10.2 |
| Go | Go 1.22+ | — | Go modules |
| Rust | Rust 2024 edition | MSRV 1.85+ | Cargo |
| Node | TypeScript 5.8 | Node 22+ | npm |
| Python | Python 3.9 | CPython 3.9+ | setuptools 68+ |
- The exact version for each binding is governed by that project's
  configuration file.
  - C++: `CMakeLists.txt`
  - .NET: `Zlink.csproj` (`PackageId`/`RootNamespace`: `Systems.Zlink`)
  - Java: `build.gradle`, `gradle-wrapper.properties`
  - Go: `go.mod`
  - Node: `package.json`, `tsconfig.json`
  - Python: `pyproject.toml`

## API Reference

Each binding generates its API reference with that language's standard
documentation tool.

| Binding | Doc tool | Generate command | Output location |
|---------|-----------|-----------|-----------|
| C++ | Doxygen | `doxygen Doxyfile` | `cpp/doxygen/html/` |
| Java | Javadoc (Gradle) | `./gradlew javadoc` | `java/build/docs/javadoc/` |
| Python | Sphinx + autodoc | `sphinx-build -b html docs docs/_build/html` | `python/docs/_build/html/` |
| Node | TypeDoc | `npx typedoc` | `node/typedoc/html/` |
| .NET | DocFX | `docfx docfx.json` | `dotnet/_site/` |
| Go | godoc / pkgsite | `go doc ./...` | (a dynamic server) |
| Rust | rustdoc | `cargo doc --no-deps` | `rust/target/doc/zlink/` |

- Run the generate command from each binding's directory.
- Exclude the output directory from tracking with `.gitignore`.
- Each binding's `README.*.md` file specifies the detailed generation
  procedure and scope.

## Disconnecting A Peer By Routing ID

- Every binding exposes core's peer-rid disconnect surface for a
  connectable raw socket type.
- The raw socket API maps to `zlink_disconnect_rid()`, and the SpotNode
  API maps to `zlink_spot_node_disconnect_peer_rid()`.
- `StreamSocket` is bind-only and does not expose peer-rid disconnect.
- A Spot facade type also does not expose a separate peer-rid disconnect
  method, because peer mesh ownership belongs to SpotNode.

| Language | Raw socket name | SpotNode name |
|---|---|---|
| C | `zlink_disconnect_rid` | `zlink_spot_node_disconnect_peer_rid` |
| C++ | `disconnect_rid` | `disconnect_peer_rid` |
| Python | `disconnect_rid` | `disconnect_peer_rid` |
| Node | `disconnectRid` | `disconnectPeerRid` |
| Go | `DisconnectRID` | `DisconnectPeerRID` |
| Rust | `disconnect_rid` | `disconnect_peer_rid` |
| Java | `disconnectRid` | `disconnectPeerRid` |
| .NET | `DisconnectRid` | `DisconnectPeerRid` |

A binding must expose `ZLINK_OPT_RID_DUPLICATE_POLICY`,
`ZLINK_RID_DUPLICATE_REJECT`, `ZLINK_RID_DUPLICATE_HANDOVER`, and the
connect result values `NOT_FOUND`, `CONFLICT`, `BUSY`, using each
language's usual enum/error mapping style.

- The C binding exposes `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES` value
  `0x3034` through the native socket option contract, and
  `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` value `18` through the context
  option contract.
- A higher-level binding must expose this capability as a typed context
  option facade.
- The public facade of a socket, SpotNode, or Spot does not add a
  per-message-unit option.
- If a raw socket path is kept for compatibility, it must be clearly
  separated from the canonical API, not used in new documentation/
  samples/tests, and must preserve the C contract as-is (`int` bytes, a
  raw default of `0`, a negative value fails with `EINVAL`).

## Related Documents
- `bindings/cpp/`
- `bindings/dotnet/`
- `bindings/java/`
- `bindings/go/`
- `bindings/rust/`
- `bindings/node/`
- `bindings/python/`

## Core API Surface 6.0.0 Alignment

- Actor create and join payloads use an aggregate multipart payload.
- The public binding API takes a message collection for remote actor
  create, actor join, actor join receive, and actor join reply.
- A single-message convenience path is allowed only when that
  language's README explicitly decides to keep that convenience
  surface. Otherwise, it's removed while cleaning up toward the
  canonical multipart path during breaking alignment.
- Even when kept, it must call the multipart path internally, and an
  empty payload must remain distinguishable from a single empty
  message.
- An admission handler receives a borrowed payload view valid only
  during the callback.

The public Registry scalar setting was removed together with the public
Discovery/Registry C API in core 8.4.3. A binding must not keep a
registry option surface, a named registry setter, or a compatibility
alias as current public API.

## Spot Route Bridge API

- A binding must expose `SpotRouteBridge`, or an equivalent typed
  handle, so `SpotNode` doesn't own the channel socket.
- The bridge references a `ROUTER` socket owned by the caller/channel
  runtime, sending a Spot route packet, or handing a SPOT relay packet
  received in the channel receive loop over to SpotNode.
- Closing the bridge does not close the registered channel socket.

A per-language API must not omit the following meaning.

- `createRouteBridge(options)` or an equivalent constructor
- `attachRouterChannel(channelName, routerSocket)`
- `sendToSpot(targetNode, targetSpot, parts)`
- `requestToSpot(targetNode, targetSpot, parts, replyHandler, timeout)`
- `handleRouterReceived(channelName, received)`
- `close` or `dispose`

`timeout == 0` uses the bridge's default timeout. When
`handleRouterReceived` returns a handled result, the binding must
clearly express to the caller that payload ownership has moved to the
bridge.

The old C API that attaches a router channel peer directly to SpotNode
is not part of the public contract. A framework adapter must not use
that path in a new implementation.
