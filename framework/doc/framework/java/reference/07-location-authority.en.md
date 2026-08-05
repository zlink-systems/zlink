# 07. Location authority

[Reference index](README.en.md)

This category covers Location/Relocation Store registration, `ZLinkLocationOptions` tuning, and
the entry points `ZLinkLocationReadiness` and `ZLinkLocationRuntimeQuery` provide. The exact
signatures are owned by the
[Java Location/Relocation exact interface](../../common/spec/server/languages/java/interfaces/location-maintenance.en.md)
(Korean-only).

---

## Location/Relocation Store registration (configuration time)

A host that uses distributed discovery, Instance Spot cold activation, or Actor/Spot relocation
registers a Store implementation at the root.

```java
options.addLocationStore(new ZLinkRedisLocationStore(
    new ZLinkRedisLocationOptions()
        .setConnectionString("redis-host:6379")
        .setKeyPrefix("zlink:game:location")));

options.addRelocationStore(new ZLinkRedisRelocationStore(
    new ZLinkRedisRelocationOptions()
        .setConnectionString("redis-host:6379")
        .setKeyPrefix("zlink:game:relocation")));
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.addLocationStore(ZLinkLocationStore)` | Without it, distributed discovery/relocation is unavailable | One Store providing exact read, conditional atomic write (`write`), and bounded prefix scan (`scan`) |
| `.addRelocationStore(ZLinkRelocationStore)` | Required if any factory uses `recreateOnRelocation()`/`preserveStateWith(...)`, or any Instance Spot factory exists | One Store that stores immutable relocation payloads under references the Framework issues |
| `ZLinkRedisLocationOptions.keyPrefix` / `ZLinkRedisRelocationOptions.keyPrefix` | A valid configuration must specify a non-empty value (and the two must differ if they share the same Redis) | The Redis key namespace |
| `.setConnectionString(value)` | Required | The Redis connection setting |
| `.setOperationTimeout(value)` | Implementation default | The provider I/O upper bound |

**Completion result.** Registers synchronously with no return value. Each role registers exactly
one — registering the same role twice, or missing a required Store, surfaces as a configuration
error in startup validation. If `Store` implements `AutoCloseable`, the Framework closes it
exactly once, after first shutting down the dependent runtime.

**When to use.** A node that only uses manual peers and needs no distributed location feature can
start by omitting this entry. Besides the official Redis provider, another provider implementing
the same `ZLinkLocationStore`/`ZLinkRelocationStore` (depending only on the opt-in artifact
`zlink-framework-provider-abstractions`) can also be registered. After registration, the
application does not call Store operations directly, nor swap the Store.

---

## `configureLocations()` (configuration time)

Tunes owner lease, polling, and the relocation concurrency cap.

```java
ZLinkLocationOptions locations = options.configureLocations();
locations.setOwnerLeaseTtl(Duration.ofSeconds(20));
locations.setMaxConcurrentRelocationCaptures(16);
```

**Options.** Commonly tuned values are as follows.

| Property | Default | Meaning |
| --- | --- | --- |
| `ownerLeaseRenewInterval` / `ownerLeaseTtl` / `ownerLeaseFencingMargin` / `ownerLeaseRenewTimeout` | 5s / 15s / 5s / 3s | The owner lease's renewal interval and TTL. Must satisfy `renewInterval + renewTimeout < ttl - fencingMargin` |
| `pollingInterval` | 1 second | The Store status-check interval |
| `storeFailureGrace` | 30 seconds | The grace period tolerating a Store failure |
| `routeCacheMaxAge` / `messageFollowDuration` | 15s / 30s | `Duration.ZERO` disables the feature. If both are positive, cache age must be at least 5 seconds smaller than message follow duration |
| `maxActiveOutboundRelocations` / `maxActiveInboundRelocations` | 64 / 64 | The cap on concurrently in-progress relocation units |
| `maxConcurrentRelocationCaptures` / `maxConcurrentRelocationRestores` | 8 / 8 | The cap on concurrently executable Capture/Restore callbacks |
| `maxRelocationPayloadInFlightBytes` | 268,435,456 | The process-wide cap on encoded relocation payload in-flight |

**Completion result.** A synchronous getter/setter. If the lease/polling values are 0 or below,
or violate the inequality above, it surfaces in startup validation. A value change while running
applies only to new relocation admissions.

**When to use.** Adjust this only when the defaults do not fit the deployment environment
(network latency, Store response time).

---

## `isPeerReady` (ZLinkLocationReadiness)

Checks whether a peer of a specific MeshName/role (optionally a specific node) is ready.

```java
boolean ready = locationReadiness.isPeerReady("play", ZLinkLocationRole.SPOT, null)
    .toCompletableFuture().get();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `nodeRid` | `null` (based on the entire role) | Narrows the check to a specific node |

**Completion result.** Returns `CompletionStage<Boolean>`. Reports only readiness, with no
separate failure kind.

**When to use.** Use this for startup ordering control or a health check that waits until a peer
of a specific role is ready.

---

## `getStatus` (ZLinkLocationRuntimeQuery)

Checks the Location runtime's own status (Store connection, owner lease renewal).

```java
ZLinkLocationRuntimeStatus status = locationQuery.getStatus().toCompletableFuture().get();
```

**Options.** This entry point has no modifiers.

**Completion result.** Returns `ZLinkLocationRuntimeStatus`. It consists of fields indicating
Store connection and owner lease renewal status.

**When to use.** Use this to diagnose the health of the Location infrastructure itself. Use
`isPeerReady` to check whether a specific peer is ready.

---

## `listTopology` / `listServiceSummaries` (ZLinkLocationRuntimeQuery)

Queries registered node topology or per-MeshName service summaries, page by page.

```java
ZLinkLocationPage<ZLinkLocationTopologyEntry> page = locationQuery
    .listTopology(new ZLinkLocationTopologyFilter("play", null, ZLinkTopologyState.READY),
                  new ZLinkPageRequest(200, null))
    .toCompletableFuture().get();
```

**Options.** Both calls take the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| filter (`ZLinkLocationTopologyFilter`/`ZLinkLocationServiceSummaryFilter`) | Everything (every field `null`) | Narrows results by MeshName/NodeRid/State |
| `page`'s page size | 100 | Range 1..1000 |
| `page`'s continuation token | `null` (first page) | The opaque token the previous response returned. The application does not interpret it directly or reuse it in a different query |

**Completion result.** Returns `ZLinkLocationPage<T>`. A `null` continuation token means the last
page. Internal information such as Store key/version, owner lease generation, or the descriptor
payload is not returned.

**When to use.** Use this in an operational tool to query registered nodes or service status in a
human-readable form. Use the status-query entry in the topology-discovery category for
real-time availability of a single MeshName/ChannelName.

---

See the
[Java Location/Relocation exact interface](../../common/spec/server/languages/java/interfaces/location-maintenance.en.md)
(Korean-only) for the full rationale.
