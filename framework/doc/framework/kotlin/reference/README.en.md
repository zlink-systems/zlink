# Kotlin Framework Reference

The writing rules follow the
[Reference-writing guide](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This tree reuses the same 8 categories and order as the dotnet reference (the
parity-reference lane).

`zlink-framework-kotlin` is a thin coroutine idiom layer that reuses the Java `zlink-framework`
runtime as-is ([Kotlin doc §0](../README.en.md#0-the-kotlin-surface-at-a-glance)). So this
reference documents **only the entry points that call in a different shape from Java** —
configuration entries that call the Java builder directly as-is (topology registration, Store
registration, `configureLocations()`, etc.) are not recreated here; instead this document points
directly at the corresponding entry in the
[Java reference](../../java/reference/README.ko.md) (Korean-only). Contract content such as
completion kinds, defaults, and ranges are all identical to Java and are not repeated in this
document.

The exact signatures are owned by the
[Kotlin exact interface](../../common/spec/server/languages/kotlin/interfaces/README.ko.md)
(Korean-only).

## Category

| Category | Status | Kotlin-specific entry points |
|---|---|---|
| [Host lifecycle](01-host-lifecycle.en.md) | Drafted | `CompletionStage<T>.await()` bridge |
| [Topology discovery](02-topology-discovery.en.md) | Drafted | `routeMesh { }`/`channel { }` DSL, `useCoroutineHandlers(...)` |
| [Messaging execution](03-messaging-execution.en.md) | Drafted | `ZLinkKotlinClient`/`ZLinkKotlinRouteClient`/`ZLinkKotlinFanoutClient`'s `await()`/`yield()` |
| [Spot instance](04-spot-instance.en.md) | Drafted | `ZLinkSuspendingSpot`/`ZLinkSuspendingEntrySpot`/`ZLinkSuspendingInstanceSpot`, `ZLinkKotlinSpotManager` |
| [Actor relocation](05-actor-relocation.en.md) | Drafted | `ZLinkSuspendingActor`/`ZLinkSuspendingActorFactory`, `ZLinkKotlinActorManager`/`ZLinkKotlinActorClient` |
| [Stream session](06-stream-session.en.md) | Drafted | `ZLinkSuspendingSession`, `ZLinkKotlinSessionClient`, `bindOrGetActor(...)` |
| [Location authority](07-location-authority.en.md) | Drafted | `suspend fun status()`/`listTopology(...)`, `Flow<T>` projection |
| [Observability diagnostics](08-observability-diagnostics.en.md) | Drafted | `onMessageFlow { }`, reuses Java `ZLinkFrameworkErrorKind` |

ko and en are both complete. This document tree is wired into `mkdocs.yml` nav.
