# 07. Location authority

[Reference index](README.en.md)

This category covers Location/Relocation Store registration, `ZLinkLocationOptions` tuning, and
the entry points `ZLinkLocationReadiness` and `ZLinkLocationRuntimeQuery`
(`ZLINK_LOCATION_RUNTIME_QUERY`) provide. The exact signatures are owned by the
[Location/Relocation provider exact interface](../../common/spec/server/languages/node/interfaces/08-location-maintenance.en.md),
the
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.en.md),
and the
[Location operational query and observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
(Korean-only).

---

## Location/Relocation Store registration (configuration time)

A host that uses distributed discovery, Instance Spot cold activation, or Actor/Spot relocation
registers a Store implementation at the root.

```ts
zlinkFramework()
  .addLocationStore(new ZLinkRedisLocationStore({
    url: "redis://redis-host:6379",
    keyPrefix: "zlink:game:location",
  }))
  .addRelocationStore(new ZLinkRedisRelocationStore({
    url: "redis://redis-host:6379",
    keyPrefix: "zlink:game:relocation",
  }));
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.addLocationStore(store: ZLinkLocationStore)` | Without it, distributed discovery/relocation is unavailable | One Store providing exact read, conditional atomic write (`write`), and bounded prefix scan (`scan`) |
| `.addRelocationStore(store: ZLinkRelocationStore)` | Required if any factory uses `recreateOnRelocation()`/`preserveStateWith(...)`, or any Instance Spot factory exists | One Store that stores immutable relocation payloads under references the Framework issues |
| `ZLinkRedisLocationOptions.keyPrefix` / `ZLinkRedisRelocationOptions.keyPrefix` | A valid configuration must specify a non-empty value (and the two must differ if they share the same Redis) | The Redis key namespace |
| `.url` or `.client`/`.clientOptions` | Required (at least one) | The Redis connection setting |
| `.operationTimeoutMs` | Implementation default | The provider I/O upper bound |

**Completion result.** Registers synchronously with no return value. Each role registers exactly
one — registering the same role twice, or missing a required Store, surfaces as a configuration
error in startup validation. If a Store has `dispose()`, the Framework first shuts down the
dependent runtime and then calls it exactly once.

**When to use.** A node that only uses manual peers and needs no distributed location feature can
start by omitting this entry. Besides the official Redis provider, another provider implementing
the same `ZLinkLocationStore`/`ZLinkRelocationStore` (depending only on the opt-in package
`@zlink-systems/framework-provider-abstractions`) can also be registered.

---

## `configureLocations()` (configuration time)

Tunes owner lease, polling, and the relocation concurrency cap.

```ts
zlinkFramework().configureLocations()
  .ownerLeaseTtlMs(20_000)
  .maxConcurrentRelocationCaptures(16);
```

**Options.** Commonly tuned values are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.ownerLeaseRenewIntervalMs(value)` / `.ownerLeaseTtlMs(value)` / `.ownerLeaseFencingMarginMs(value)` / `.ownerLeaseRenewTimeoutMs(value)` | 5000 / 15000 / 5000 / 3000 | The owner lease's renewal interval and TTL (ms). Must satisfy `renewInterval + renewTimeout < ttl - fencingMargin` |
| `.pollingIntervalMs(value)` | 1000 | The Store status-check interval |
| `.storeFailureGraceMs(value)` | 30000 | The grace period tolerating a Store failure |
| `.routeCacheMaxAgeMs(value)` / `.messageFollowDurationMs(value)` | 15000 / 30000 | `0` disables the feature. If both are positive, cache age must be at least 5 seconds smaller than message follow duration |
| `.maxActiveOutboundRelocations(value)` / `.maxActiveInboundRelocations(value)` | 64 / 64 | The cap on concurrently in-progress relocation units |
| `.maxConcurrentRelocationCaptures(value)` / `.maxConcurrentRelocationRestores(value)` | 8 / 8 | The cap on concurrently executable Capture/Restore callbacks |
| `.maxRelocationPayloadInFlightBytes(value)` | 268,435,456 | The process-wide cap on encoded relocation payload in-flight |

**Completion result.** Each modifier is a synchronous fluent call returning
`ZLinkLocationOptions`. If the lease/polling values are 0 or below, or violate the inequality
above, it surfaces in startup validation. A value change while running applies only to new
relocation admissions.

**When to use.** Adjust this only when the defaults do not fit the deployment environment
(network latency, Store response time).

---

## `isPeerReady` (ZLinkLocationReadiness)

Checks whether a peer of a specific MeshName/role (optionally a specific node) is ready.

```ts
const ready = await locationReadiness.isPeerReady("play", ZLinkLocationRole.Spot);
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `nodeRid` | Omitted (based on the entire role) | Narrows the check to a specific node |
| `signal` | None | Cancels only the waiter of this `Promise` |

**Completion result.** Returns `Promise<boolean>`. Reports only readiness, with no separate
failure kind.

**When to use.** Use this for startup ordering control or a health check that waits until a peer
of a specific role is ready.

---

## `getStatus` (ZLinkLocationRuntimeQuery)

Checks the Location runtime's own status (Store connection, owner lease renewal).

```ts
const status = await locationQuery.getStatus();
```

**Options.** This entry point only has an optional `signal`.

**Completion result.** Returns `ZLinkLocationRuntimeStatus`. It consists of fields indicating
Store connection and owner lease renewal status.

**When to use.** Use this to diagnose the health of the Location infrastructure itself. Use
`isPeerReady` to check whether a specific peer is ready.

---

## `listTopology` / `listServiceSummaries` (ZLinkLocationRuntimeQuery)

Queries registered node topology or per-MeshName service summaries, page by page.

```ts
const page = await locationQuery.listTopology(
  { meshName: "play", state: ZLinkLocationTopologyState.Ready },
  { pageSize: 200 },
);
```

**Options.** Both calls take the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| filter (`ZLinkLocationTopologyFilter`/`ZLinkLocationServiceSummaryFilter`) | Everything (every field omitted) | Narrows results by MeshName/NodeRid/State |
| `page.pageSize` | 100 | An integer in range 1..1000 |
| `page.continuationToken` | Omitted (first page) | The opaque token the previous response returned. The application does not interpret it directly or reuse it in a different query |

**Completion result.** Returns `ZLinkLocationPage<T>`. No continuation token means the last page.
Internal information such as Store key/version, owner lease generation, or the descriptor payload
is not returned.

**When to use.** Use this in an operational tool to query registered nodes or service status in a
human-readable form. Use the status-query entry in the topology-discovery category for
real-time availability of a single MeshName/ChannelName.

---

See the
[Location/Relocation provider exact interface](../../common/spec/server/languages/node/interfaces/08-location-maintenance.en.md)
and the
[Location operational query and observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
(Korean-only) for the full rationale.
