<!-- framework-adapter-nav:start -->
[Document List](../../../README.en.md) | [Previous: Regression Test Matrix](regression-test-matrix.en.md)<!-- framework-adapter-nav:end -->

[Spec Index](../../common/README.en.md)

[.NET Bundle](../README.en.md) | [Interfaces](../../common/spec/server/languages/dotnet/interfaces/README.en.md) | [Runtime Lifecycle](../../common/internals/README.en.md) | [Regression Matrix](regression-test-matrix.en.md)

# ZLink Framework .NET Backend Dependency Policy

## 1. Purpose

At this point, the most realistic choice is to use the `bindings/dotnet` library directly as the
backend. But the framework's public contract must not get too tangled up with this backend
implementation. Otherwise, replacing the low-level library later would inevitably break the public
API too.

This document lays out criteria that satisfy both of these at once:

- Right now, implement using `bindings/dotnet`.
- Later, the backend must be replaceable with a different low-level `.NET` library.

## 2. Basic Principles

The backend library is treated as a replaceable implementation, with the goal of stably layering the
framework's public contract on top of it. Specifically, the following four principles apply:

- The framework's public contract comes first. The backend library is treated as a replaceable
  implementation.
- The backend's service objects are not directly exposed in the public API.
- Types that tend to change along with the backend are hidden inside the framework boundary.
- The transport primitive[^transport-primitive] already left in the public contract is treated as "a
  core value with meaning independent of the backend."

## 3. Current Backend Policy

- The current backend implementation is `bindings/dotnet`.
- The framework runtime uses only the raw `DEALER`, `ROUTER`, `PUB`, `SUB`, `STREAM` socket API the
  bindings expose.
- However, by default, the framework user is not made to directly receive these objects as
  constructor parameters or public properties.

In other words, "right now, it's implemented using `bindings/dotnet`" and "the framework public API
must be exactly `bindings/dotnet`'s object model" are two different claims.

## 4. What Can Stay In The Public API

Under this document's current standard, the following types stay in the public contract as-is.

- `RoutingId`
- `Message`
- `SendFlags`
- `ActorRef`

These types are not specific runtime objects. They correspond to basic primitives with clear meaning,
like transport identity[^transport-identity], payload, submit option, and actor handle. In other
words, they're the kind of type a compatibility layer[^compatibility-layer] can be slotted in for, so
the same meaning is preserved even if the backend is replaced later.

## 5. What Must Not Leak Directly Into The Public API

The following types and object models are not directly exposed in the framework's public contract.

- raw socket instances
- `SpotNode`
- `Spot`
- lower-level objects like timers, the recv loop, and the raw socket monitor

These objects are close to backend implementation details. So it's a problem if even one of them
leaks into the public API surface[^public-surface] — the next backend replacement would immediately
become a breaking change[^breaking-change].

## 6. Diagnostic/Operational Type Policy

Values close to the lower layer, like monitoring, registry queries, and spot status, can partly stay
in the public surface. Even so, the scope is narrowed by the following principles.

- Values like the source name, timestamp, and logical event kind are treated as values whose meaning
  the framework owns.
- Native enums or raw status values, on the other hand, are kept only as optional diagnostic
  details.
- Values that are hard to keep with the same meaning when the backend is replaced are handled
  differently. In that case, a framework-defined synthetic enum[^synthetic-enum] and snapshot
  DTO[^dto] are exposed first by default, keeping native detail to a minimum.

In other words, of the two structures for the monitoring public API, the latter is safer for backend
replacement.

- "A structure that re-exports the backend's raw event as-is"
- "A structure of a typed runtime event the framework has re-interpreted once, plus native detail
  added only when truly necessary"

## 7. Layer and Adapter Selection

Do not add an adapter unconditionally between the framework and the backend. If the
binding public API has the same meaning, ownership, lifecycle, readiness, error, and
concurrency rules as the Framework operation, call that public API directly from the
binding-facing integration. If any of those rules differ, keep a semantic adapter or
port that hides the difference in one place.

The current .NET layering is judged by the following graph.

```text
+----------------------------------------------+
| Framework public/domain contract             |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Framework semantic runtime core              |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Binding-facing runtime integration           |
| direct public call or semantic adapter       |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Systems.Zlink public API                     |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Core                                         |
+----------------------------------------------+
```

The detailed type, operation, ownership, and receive-storage classification is recorded in
[runtime integration and receive ownership](runtime-integration-and-ownership.en.md).

Keep an adapter only when the code and contract demonstrate at least one of the following:

- It turns a binding fluent operation into one Framework submit result.
- It maps Framework options to binding options while hiding the decision in one place.
- It owns disposal order for a context, socket, or session.
- It maps binding events, readiness, or errors into Framework meaning.
- It composes several binding objects into one Spot, Stream, or lifecycle operation.
- It manages the lifetime of message storage handed to an asynchronous Framework queue.

A facade that forwards identical arguments and results, a one-implementation backend added
only for testability, and a wrapper that only renames binding methods do not satisfy this
criterion and should be removed. Adapters call only the bindings' public API. They do not use
the Core service C API, `NativeMethods`, non-public reflection, direct native symbols, or
service binding objects. Low-level binding types shown in samples remain separate from the
Framework public API explanation.

## 8. Replacement Rules

When replacing the low-level library, preserve the Framework public contract first. Compare
a direct binding call and a semantic adapter for the same operation, and introduce an
adapter only where the comparison proves a semantic difference. Do not promote an internal
type of the existing adapter into the new public contract.

If the binding public API lacks a required operation, do not add reflection or a raw-frame
escape hatch in the Framework. Define and test the required public binding contract first,
update the local package version, and make the Framework call only that public API. A change
to the public contract itself is reviewed separately as a breaking change.

## 9. Regression Tests

The backend dependency policy is checked along two axes: the framework's public API and the adapter
factory. There are two check criteria.

- Even if the implementation changes, the user must not need to know the backend's concrete type.
- Native binding wrapper creation must happen only inside the adapter.

| Test Case | Pass Criteria |
|---------------|-----------|
| `ScaffoldSmokeTests.PublicSurface_DoesNotExpose_BackendConcreteTypes` | Except for the allowed value types, backend concrete types do not appear in the public surface. |
| `BackendAdapterFactoryTests.BackendFactory_Creates_Channel_Spot_And_Stream_Wrappers` | The backend factory creates all of the channel, SPOT, and STREAM wrappers. |
| `BackendAdapterFactoryTests.BackendFactory_Creates_MonitoringAdapter` | The monitoring adapter creation path stays inside the backend. |
| `BackendDependencyTests.Runtime_Uses_Only_Public_Raw_Binding_Surface` | There are zero references to the service C API, private members, or reflection. |

[^public-contract]: The public contract means the API surface exposed to external users, whose compatibility must be maintained on change.
[^backend]: The backend refers to the low-level implementation the framework delegates actual behavior to. Here, the backend is `bindings/dotnet`.
[^transport-primitive]: A transport primitive is a basic value whose meaning is firmly fixed at the message-transport layer (e.g., `RoutingId`, `Message`, `SendFlags`).
[^transport-identity]: Transport identity is the value that identifies who is sending to whom at the transport layer. `RoutingId` is the representative example.
[^compatibility-layer]: A compatibility layer is intermediate code inserted so the externally visible meaning stays the same even when the internal implementation changes.
[^public-surface]: The public surface is the sum of every type, method, and attribute exposed to external users. This document spells it out as the public API surface where possible.
[^breaking-change]: A breaking change is an incompatible change that makes it impossible to rebuild or run existing user code as-is.
[^synthetic-enum]: A synthetic enum is an enum the framework redefines with its own meaning, instead of using the raw value the backend hands down as-is.
[^dto]: A DTO (Data Transfer Object) is a simple data structure used to move values between layers.
[^backend-adapter]: The backend adapter layer is the intermediate layer connecting the framework's surface with the actual low-level backend. It keeps the public API stable even when the backend changes.
[^lifecycle]: Lifecycle refers to a component's entire span from start through operation to shutdown, and what happens at each stage.

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../../../README.en.md) | [Previous: Regression Test Matrix](regression-test-matrix.en.md)<!-- framework-adapter-nav:bottom:end -->
