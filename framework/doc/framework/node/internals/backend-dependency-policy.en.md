<!-- framework-adapter-nav:start -->
[Document List](../README.en.md) | [Previous: Regression Test Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:end -->

[Spec Index](../README.en.md)

[Node.js Bundle](../README.en.md) | [Interfaces](../../common/spec/server/languages/node/interfaces/README.en.md) | [Runtime Lifecycle](../../common/internals/README.en.md) | [Regression Matrix](regression-test-matrix.en.md)

# ZLink Framework Node.js Backend Dependency Policy

## 1. Purpose

At this point, the most realistic choice is to use the `@zlink-systems/zlink` (Node binding)
library directly as the backend. But the framework's public contract must not get too tangled up
with this backend implementation. Otherwise, replacing the low-level library later would inevitably
break the public API too.

This document lays out criteria that satisfy both of these at once:

- Right now, implement using `@zlink-systems/zlink`.
- Later, the backend must be replaceable with a different low-level Node.js library.

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

- The current backend implementation is `@zlink-systems/zlink`.
- The framework runtime can internally use lower-level objects like `Discovery`, `DealerSocket`,
  `RouterSocket`, `SpotNode`, `Spot`, and `Registry`.
- However, by default, the framework user is not made to directly receive these objects as
  constructor parameters (NestJS provider injection) or public properties.

In other words, "right now, it's implemented using `@zlink-systems/zlink`" and "the framework public
API must be exactly `@zlink-systems/zlink`'s object model" are two different claims.

## 4. What Can Stay In The Public API

Under this document's current standard, the following types stay in the public contract as-is.

- `RoutingId` (a `string` alias)
- `Message` (a payload structure type, `Buffer`-based)

These types are not specific runtime objects. They correspond to basic primitives with clear meaning,
like transport identity[^transport-identity], payload, and submit option. In particular, `Message` is
not a plain alias of `@zlink-systems/zlink`'s concrete class — it is kept as a framework-owned
structure type that can read, copy, and release the payload. In other words, it's the kind of type a
compatibility layer[^compatibility-layer] can be slotted in for, so the same meaning is preserved even
if the backend is replaced later.

## 5. What Must Not Leak Directly Into The Public API

The following types and object models are not directly exposed in the framework's public contract.

- `DealerSocket`
- `RouterSocket`
- `Discovery`
- `SpotNode`
- `Spot`
- `Registry`
- lower-level objects like timers, the recv loop, and the raw socket monitor

These objects are close to backend implementation details. So it's a problem if even one of them
leaks into the public surface[^public-surface] — the next backend replacement would immediately
become a breaking change[^breaking-change].

## 6. Diagnostic/Operational Type Policy

Values close to the lower layer, like monitoring, location runtime queries, and Spot status, can
partly stay in the public surface. Even so, the scope is narrowed by the following principles.

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

- The framework has an internal backend adapter layer[^backend-adapter]. This adapter implements the
  port interfaces in `runtime/backend/contracts` (`ZLinkBackendAdapterFactory`,
  `ZLinkChannelBackendAdapter`, `ZLinkSpotBackendAdapter`, `ZLinkStreamBackendAdapter`,
  `ZLinkMonitoringBackendAdapter`) on top of `@zlink-systems/zlink`. The actual implementation is in
  `runtime/backend/node` (`ZLinkNodeBackendAdapterFactory`).
- The framework's own responsibilities — registration, lifecycle[^lifecycle], monitoring, query —
  are handled by the framework service (a NestJS provider). Actual backend calls are handled by the
  adapter layer.
- Even if sample documentation directly shows a low-level binding type, that explanation is kept
  separate from the description of the framework's public surface.

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
| `backend-public-api-only.test.js` › `framework contract surface does not alias binding concrete types` | Except for the allowed value types (`RoutingId`/`Message`/`SendFlags`), backend concrete types (`DealerSocket`/`RouterSocket`/`SpotNode`/`Spot`, etc.) do not appear in the public surface. |
| `backend-contract.test.js` › `backend adapter factory exposes the supported backend adapters` | The backend factory creates the channel, SPOT, STREAM, and monitoring adapters, and does not expose a Registry adapter. |
| `backend-public-api-only.test.js` › `framework packages only depend on binding public entry points` | Including the monitoring adapter creation path, the framework uses only binding public entry points. |

[^public-contract]: The public contract means the API surface exposed to external users, whose compatibility must be maintained on change.
[^backend]: The backend refers to the low-level implementation the framework delegates actual behavior to. Here, the backend is `@zlink-systems/zlink` (the Node binding).
[^transport-primitive]: A transport primitive is a basic value whose meaning is firmly fixed at the message-transport layer (e.g., `RoutingId`, `Message`, `SendFlags`).
[^transport-identity]: Transport identity is the value that identifies who is sending to whom at the transport layer. `RoutingId` is the representative example.
[^compatibility-layer]: A compatibility layer is intermediate code inserted so the externally visible meaning stays the same even when the internal implementation changes.
[^public-surface]: The public surface is the sum of every type, method, and decorator exposed to external users.
[^breaking-change]: A breaking change is an incompatible change that makes it impossible to rebuild or run existing user code as-is.
[^synthetic-enum]: A synthetic enum is an enum the framework redefines with its own meaning, instead of using the raw value the backend hands down as-is.
[^dto]: A DTO (Data Transfer Object) is a simple data structure used to move values between layers.
[^backend-adapter]: The backend adapter layer is the intermediate layer connecting the framework's surface with the actual low-level backend. It keeps the public API stable even when the backend changes.
[^lifecycle]: Lifecycle refers to a component's entire span from start through operation to shutdown, and what happens at each stage.

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../README.en.md) | [Previous: Regression Test Matrix](regression-test-matrix.en.md)
<!-- framework-adapter-nav:bottom:end -->
