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

## 7. Implementation Guidelines

An adapter layer always sits between the framework and the backend, with roles cleanly separated.

- The framework has an internal backend adapter layer[^backend-adapter].
- The framework's own responsibilities — registration, lifecycle[^lifecycle], monitoring, query —
  are handled by the framework service. Actual backend calls are handled by the adapter layer.
- The adapter calls only the bindings' public raw socket API. It does not use the Core service C
  API, `NativeMethods`, non-public reflection, direct native symbol calls, or service binding
  objects.
- Even if sample documentation directly shows a low-level binding type, that explanation is kept
  separate from the description of the framework's public API surface.

## 8. Rules For Replacement

When replacing the low-level library later, the following order applies.

1. Keep the framework's public contract unchanged first.
2. Plug the existing backend adapter and the new backend adapter side by side behind the same
   contract.
3. Confirm that the primitives remaining in the public API can still be kept as-is on the new
   backend.
4. For a type that can't be kept, don't eliminate it right away as part of the backend replacement —
   introduce a framework wrapper first, then replace it.

In other words, a backend replacement is treated fundamentally as a replacement of the adapter layer.
Replacing the public API itself is kept as a separate breaking-change effort, done independently.

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
