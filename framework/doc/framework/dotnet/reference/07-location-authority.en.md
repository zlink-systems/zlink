# 07. Location authority

[Reference index](README.en.md)

This category covers Location·Relocation Store registration, tuning `ZLinkLocationOptions`, and
the entry points `IZLinkLocationReadiness` and `IZLinkLocationRuntimeQuery` provide. The exact
signatures are owned by the
[Location configuration and operations exact interface](../../common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md)
and the
[Official Redis Store exact interface](../../common/spec/server/languages/dotnet/interfaces/08-location-provider-redis.ko.md)
(both Korean-only).

---

## Location·Relocation Store registration (configuration time)

A host that uses distributed discovery, Instance Spot cold activation, or Actor·Spot relocation
registers Store implementations at the root.

```csharp
services.AddZLinkFramework(options =>
{
    options.AddLocationStore(
        new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:location",
        })); // registers a provider that stores small opaque location records

    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:relocation",
        })); // registers immutable relocation payloads as a separate capability
});
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.AddLocationStore(IZLinkLocationStore)` | without it, distributed discovery·relocation is unavailable | one Store providing exact read, conditional atomic batch, and bounded snapshot scan |
| `.AddRelocationStore(IZLinkRelocationStore)` | required if any `RecreateOnRelocation`/`PreserveStateWith` factory or Instance Spot factory exists | one Store that stores immutable relocation payloads at a Framework-issued reference |
| `ZLinkRedisLocationOptions.KeyPrefix` / `ZLinkRedisRelocationOptions.KeyPrefix` | the code initializer is an empty string, but a valid configuration must set a non-empty value (the two must differ if they share one Redis) | the Redis key namespace |
| `.ConnectionString` or `.ConfigurationOptions` | required (one of the two) | the Redis connection setting. If both are specified, `ConfigurationOptions` is used |

**Completion.** Registers synchronously with no return value. Register exactly one Store per
role — registering the same role twice, or missing a required Store, surfaces as
`ZLinkConfigurationException` during host startup validation. An `OperationTimeout` of 0 or less
is rejected with `ArgumentException` before provider I/O.

**When to use it.** A node that only uses manual peers and needs no distributed location
capability can omit this entry and still start. Besides the official Redis provider, any other
provider implementing the same `IZLinkLocationStore`/`IZLinkRelocationStore` can also be
registered. After registration, the application does not call Store operations directly or
swap·dispose the Store.

---

## `ConfigureLocations()` (configuration time)

Tunes the owner lease, polling, and relocation concurrency caps.

```csharp
services.AddZLinkFramework(options =>
{
    ZLinkLocationOptions locations = options.ConfigureLocations();
    locations.OwnerLeaseTtl = TimeSpan.FromSeconds(20);
    locations.MaxConcurrentRelocationCaptures = 16;
});
```

**Options.** Commonly tuned values:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `OwnerLeaseRenewInterval` / `OwnerLeaseTtl` / `OwnerLeaseFencingMargin` / `OwnerLeaseRenewTimeout` | 5s / 15s / 5s / 3s | the owner lease renewal interval and validity period. Must satisfy `OwnerLeaseRenewInterval + OwnerLeaseRenewTimeout < OwnerLeaseTtl - OwnerLeaseFencingMargin` |
| `PollingInterval` | 1 second | the interval for checking Store state |
| `StoreFailureGrace` | 30 seconds | the grace period for tolerating Store failure |
| `RouteCacheMaxAge` / `MessageFollowDuration` | 15s / 30s | 0 turns the feature off. If both are positive, cache age must be at least 5 seconds less than Message Follow duration |
| `MaxActiveOutboundRelocations` / `MaxActiveInboundRelocations` | 64 / 64 | the cap on relocation units that can be in progress concurrently |
| `MaxConcurrentRelocationCaptures` / `MaxConcurrentRelocationRestores` | 8 / 8 | the cap on Capture·Restore callbacks that can run concurrently |
| `MaxRelocationPayloadInFlightBytes` | 268,435,456 | the process-wide cap on encoded relocation payload in flight |

**Completion.** A synchronous setting. Lease·polling values of 0 or less, or violating the
inequality above, surface during host startup validation. A value change during execution
applies only to new relocation admissions.

**When to use it.** Adjust it only when the defaults do not fit the deployment environment
(network latency, Store response time).

---

## `IsPeerReadyAsync`

Checks whether a peer for a specific MeshName·role (optionally a specific node) is ready.

```csharp
bool ready = await locationReadiness.IsPeerReadyAsync(
    "play",
    ZLinkLocationRole.Spot,
    nodeRid: null,
    ct);
```

**Options.** The following modifier attaches to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `nodeRid` | `null` (checks against the whole role) | narrows the check to a specific node |

**Completion.** Returns a `bool`. It reports only readiness, with no separate failure kind.

**When to use it.** Use it for startup-order control or a health check that waits until a
specific role's peer is ready.

---

## `GetStatusAsync`

Checks the Location runtime's own state (Store connection, owner lease renewal).

```csharp
ZLinkLocationRuntimeStatus status = await locationQuery.GetStatusAsync(ct);
bool healthy = status.StoreHealthy && status.OwnerLeaseHealthy;
```

**Options.** This entry point has no modifiers — it only takes a `CancellationToken`.

**Completion.** Returns a `ZLinkLocationRuntimeStatus`. `StoreHealthy` and `OwnerLeaseHealthy`
report the Store connection and owner lease renewal state respectively, and
`LastRefreshAt`/`OwnerLeaseRenewedAt` give the last refresh time.

**When to use it.** Use it to diagnose the health of the Location infrastructure itself. For a
specific peer's readiness, use `IsPeerReadyAsync`.

---

## `ListTopologyAsync` / `ListServiceSummariesAsync`

Queries registered node topology or per-MeshName service summaries page by page.

```csharp
ZLinkLocationPage<ZLinkLocationTopologyEntry> page = await locationQuery.ListTopologyAsync(
    new ZLinkLocationTopologyFilter(MeshName: "play", State: ZLinkLocationTopologyState.Ready),
    new ZLinkPageRequest(PageSize: 200),
    ct);
```

**Options.** Both calls take the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| filter (`ZLinkLocationTopologyFilter`/`ZLinkLocationServiceSummaryFilter`) | everything (all fields `null`) | narrows results by MeshName·NodeRid·State |
| `page.PageSize` | 100 | range 1..1000 |
| `page.ContinuationToken` | `null` (first page) | the opaque token a previous response returned. The application does not interpret it or reuse it for a different query |

**Completion.** Returns a `ZLinkLocationPage<T>`. A `null` `ContinuationToken` means the last
page. It does not return internal information such as Store key·version, owner lease
generation, or descriptor payload.

**When to use it.** Use it in operational tooling to query registered nodes or service status in
a human-readable form. For real-time availability of a single MeshName·ChannelName, use the
topology-discovery category's status-query entry.

---

The full basis is the
[Location configuration and operations exact interface](../../common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md) and the
[Official Redis Store exact interface](../../common/spec/server/languages/dotnet/interfaces/08-location-provider-redis.ko.md)
(both Korean-only).
