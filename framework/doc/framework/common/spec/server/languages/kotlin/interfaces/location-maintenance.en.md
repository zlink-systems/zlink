# Kotlin Location/Relocation Public Interface

[Kotlin exact interface list](README.en.md) ·
[Java Location/Relocation Contract](../../java/interfaces/location-maintenance.en.md)

Kotlin uses the Java runtime and provider SPI unchanged. It doesn't
define a separate Kotlin Store interface, abstract Store base class, or
Redis wrapper. A provider implements Java's `ZLinkLocationStore` or
`ZLinkRelocationStore` and registers it with the existing Java
`ZLinkFrameworkOptions.addLocationStore(...)` and
`addRelocationStore(...)`. Both Stores and their primitives are taken
from Java's opt-in
`systems.zlink:zlink-framework-provider-abstractions` artifact.

## Provider Contract

The following boundaries of the Java contract apply to Kotlin unchanged.

- The Location Store only provides opaque key/value read, atomic batch
  write including a version condition, and bounded snapshot scan.
- The key/version/cursor, value/batch/scan ranges, and provider-clock-
  based TTL semantics aren't changed.
- The Relocation Store stores an immutable blob at a reference the
  framework issued in advance.
- The state handoff payload of an Actor/Spot relocation isn't stored in
  this Store — it is transferred as chunks directly from source to
  target. The steady-state responsibilities remaining with the Store are
  the Instance Spot cold activation record and the terminal record of a
  pending request completed after relocation.
- Retrying with the same reference and same bytes is AlreadyStored;
  different bytes is Conflict.
- One blob is at most 64 MiB, and the framework composes a logical
  stream of at most 256 GiB from at most 4,096 chunks.
- The ownership and close order after Store registration are the same
  as the Java contract. The provider manages the shared connection
  lease.

Writing a Kotlin provider also implements the Java `CompletionStage`
SPI. A separate `suspend` Store interface isn't duplicated. So coroutine
scheduling doesn't change the atomic commit boundary, cancellation
reconciliation, or return buffer lifetime.

Authority, owner lease, reservation, capacity, aggregate, fence, and
relocation phase are framework-private records. They aren't re-exposed
as a Kotlin public declaration or provider result type. Redis key
layout, Lua script, private encoding, retry, and connection reference
count also aren't made public.

## Coroutine Operational Query

The Kotlin package only provides a coroutine projection for the
operational query the application uses.

```kotlin
suspend fun ZLinkLocationRuntimeQuery.status(): ZLinkLocationRuntimeStatus

suspend fun ZLinkLocationRuntimeQuery.listTopology(
    filter: ZLinkLocationTopologyFilter,
    page: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
): ZLinkLocationPage<ZLinkLocationTopologyEntry>

suspend fun ZLinkLocationRuntimeQuery.listServiceSummaries(
    filter: ZLinkLocationServiceSummaryFilter,
    page: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
): ZLinkLocationPage<ZLinkLocationServiceSummary>

suspend fun ZLinkLocationRuntimeQuery.findActorLocation(
    actorId: String,
): ZLinkLocationObjectEntry?

suspend fun ZLinkLocationRuntimeQuery.findSpotLocation(
    spotId: String,
): ZLinkLocationObjectEntry?

suspend fun ZLinkLocationRuntimeQuery.listObjectLocations(
    filter: ZLinkLocationObjectFilter,
    page: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
): ZLinkLocationPage<ZLinkLocationObjectEntry>

fun ZLinkLocationRuntimeQuery.topology(
    filter: ZLinkLocationTopologyFilter,
    pageSize: Int = 100,
): Flow<ZLinkLocationTopologyEntry>
```

The query projection keeps the bounded page and Java result type. An exact
lookup returns `null` for Missing, a `CREATING` entry for Creating, a `READY`
entry for Ready, and an `UNAVAILABLE` entry when the current owner is unavailable
after commit. Spot exact lookup treats User Spot and Instance Spot under the same
Spot-ID lookup contract. An object list requires kind, and takes stable type and
MeshName as optional filters. Its page size is `1..1000`, the encoded page is at
most 4 MiB, and the continuation token is an opaque value issued by the query. A
Store query failure is Java `ZLinkFrameworkErrorKind.UNAVAILABLE` and does not
return a partial page. Raw Spot/Actor authority rows, Store keys,
provider versions, and scan cursors aren't added to the application query result.

## Redis Extension

The Kotlin application and provider use Java's `ZLinkRedisLocationStore`,
`ZLinkRedisRelocationStore`, and each options class unchanged. A
Kotlin-only registration helper or a wrapper bundling both Stores isn't
provided. The Redis public surface is limited to the two public Store
classes' minimal constructor, and connection/key namespace/operation
timeout options.
