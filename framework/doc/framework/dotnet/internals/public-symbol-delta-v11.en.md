<!-- framework-adapter-nav:start -->
[Document List](../../../README.en.md) | [Runtime Lifecycle](../../common/internals/README.en.md)
<!-- framework-adapter-nav:end -->

# .NET v11 Public Boundary

[Exact Interface](../../common/spec/server/languages/dotnet/interfaces/README.en.md) ·
[Runtime Lifecycle](../../common/internals/README.en.md)

## 1. Purpose

This document classifies what remains in the `.NET` public boundary after moving the Core service
runtime into the Framework. The exact signatures and enum numbers are owned by the
[exact interface](../../common/spec/server/languages/dotnet/interfaces/README.en.md).

## 2. The Core Migration Boundary

The Framework uses only the bindings' public raw socket API. Core service objects, private members,
reflection, and native symbol bypass options are not exposed in the public API.

| Category | v11 Public Boundary |
|---|---|
| Application messaging | Channel, Spot, Actor, and STREAM's typed builder/handler |
| Object location | global `SpotId`, `ActorId`, and immutable `SpotRef`/`ActorRef` |
| Host lifecycle | `IZLinkFrameworkRuntime.RelocateAsync(...)`, `ShutdownAsync(...)`, and status/result |
| Provider SPI | `IZLinkLocationStore`, `IZLinkRelocationStore` |
| Redis extension | `ZLinkRedisLocationStore`, `ZLinkRedisRelocationStore` |
| Internal only | authority record, reservation, recovery state machine, wire command, and raw socket |

An Actor/Spot application doesn't need to know about provider records or wire commands. A provider
also doesn't implement the Framework's private record types — it stores only the keys and opaque
bytes the two Store SPIs receive.

## 3. Relocation And Shutdown

`RelocateAsync(...)` moves a stateful workload and keeps the infrastructure in the `Relocated`
state. `ShutdownAsync(...)` shuts down the host without starting workload relocation. If continuity
is needed, the Application confirms relocation success and then calls shutdown separately.

There are two relocation modes.

| Mode | Target |
|---|---|
| `PlannedMaintenance` | an eligible node of the same application version |
| `RollingUpdate` | an eligible node of the exact application version specified in the option |

An Actor/Spot factory fixes a `DisableRelocation`, `RecreateOnRelocation`, or `PreserveStateWith`
policy. A `PreserveStateWith` adapter captures/restores only opaque `byte[]` application state.
Authority phase, participant metadata, accepted journal, queue, and timer restoration are the
Framework's internal responsibility.

## 4. The Store Boundary

The Location Store atomically publishes the owner, generation, relocation phase, and payload
reference. The Relocation Store stores application state, accepted journal, queue, and timer payload
as immutable roots. No distributed transaction is required between the two Stores.

The Framework writes the payload to the Relocation Store first and verifies it. It then publishes the
reference with a single CAS on the Location Store. An unpublished payload is cleaned up by the
retention policy.

## 5. Regression Tests

| Test Case | Pass Criteria |
|---|---|
| `FrameworkRuntimeContracts.Public_values_match_the_exact_contract` | The runtime/relocation enum numbers match the exact interface. |
| `FrameworkRuntimeContracts.Relocation_and_shutdown_are_separate_host_operations` | Relocation and shutdown are separate public operations. |
| `FrameworkRuntimeContracts.Retire_surface_is_not_public` | The old host maintenance method is not re-exposed in the public interface. |
| `ProviderStoreContracts.Location_provider_exposes_only_opaque_store_operations` | The Location provider implements only opaque Store operations. |
| `ProviderStoreContracts.Relocation_provider_exposes_only_immutable_blob_operations` | The Relocation provider implements only immutable payload Store operations. |
| `ScaffoldSmokeTests.PublicSurface_DoesNotExpose_BackendConcreteTypes` | The raw backend service objects are not exposed in the application public API. |

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../../../README.en.md) | [Runtime Lifecycle](../../common/internals/README.en.md)
<!-- framework-adapter-nav:bottom:end -->
