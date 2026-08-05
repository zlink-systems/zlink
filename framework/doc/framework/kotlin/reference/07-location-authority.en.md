# 07. Location authority

[Reference index](README.en.md)

Store registration (`addLocationStore`/`addRelocationStore`), `configureLocations()`, and
`isPeerReady` use the Java types and builders directly — the exact signature and options table
follow
[Java reference 07. Location authority](../../java/reference/07-location-authority.ko.md)
(Korean-only) directly. The only things Kotlin adds are suspend extensions and `Flow`
projections over the operational query (`ZLinkLocationRuntimeQuery`). Even when implementing a
provider directly, it implements the Java `CompletionStage` SPI as-is — there is no separately
duplicated `suspend` Store interface. The exact signatures are owned by the
[Kotlin Location/Relocation exact interface](../../common/spec/server/languages/kotlin/interfaces/location-maintenance.en.md)
(Korean-only).

---

## `status()` (suspend extension)

Checks the Location runtime's own status. A suspend extension function wrapping Java's
`getStatus()`.

```kotlin
val status = locationQuery.status()
```

**Options.** This function has no modifiers.

**Completion result.** Same completion result as the Java reference's `getStatus`
(`ZLinkLocationRuntimeStatus`).

**When to use.** Same as the `getStatus` entry in the Java reference.

---

## `listTopology` / `listServiceSummaries` (suspend extension) / `topology` (Flow)

Queries registered node topology or per-MeshName service summaries page by page, or receives
them continuously as a `Flow`.

```kotlin
val page = locationQuery.listTopology(
    ZLinkLocationTopologyFilter("play", null, ZLinkTopologyState.READY),
)

// hiding pagination and iterating with a Flow
locationQuery.topology(filter, pageSize = 200).collect { entry -> ... }
```

**Options.** The `page` argument of `listTopology`/`listServiceSummaries` defaults to
`ZLinkPageRequest.firstPage()` — the remaining filter/page semantics are the same as the
`listTopology`/`listServiceSummaries` entry in the Java reference. `topology(filter, pageSize =
100)` returns `Flow<ZLinkLocationTopologyEntry>` and internally follows the continuation token to
automatically query the next page.

**Completion result.** The query projection keeps the bounded page and Java result type. It adds
no raw Spot/Actor authority row, Store key, provider version, or scan cursor.

**When to use.** Use the suspend extension when only one page of results is needed, and the
`Flow` version to iterate over every entry.

---

See the
[Kotlin Location/Relocation exact interface](../../common/spec/server/languages/kotlin/interfaces/location-maintenance.en.md)
and
[Java reference 07. Location authority](../../java/reference/07-location-authority.ko.md)
(Korean-only) for the full rationale.
