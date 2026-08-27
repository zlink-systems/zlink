# .NET Location Configuration And Operations Public Interface

[.NET per-language interface table of contents](README.en.md) · [Location Runtime](../../../05-location-relocation/01-location-runtime.en.md) ·
[Provider SPI](08-authority-relocation.en.md) · [Host Monitoring](10-topology-monitoring.en.md)

## 1. Scope

This document only defines the Location configuration, readiness, and
operational queries the application uses. The technical primitives the
Store provider implements are owned by the
[provider SPI](08-authority-relocation.en.md).

Authority key/version, owner token, descriptor record, capacity fence,
reservation, aggregate, and relocation reference are framework-internal
information, so they aren't declared in this application contract.

## 2. Location Option

```csharp
public sealed class ZLinkLocationOptions
{
 public TimeSpan OwnerLeaseRenewInterval { get; set; }
 = TimeSpan.FromSeconds(5);
 public TimeSpan OwnerLeaseTtl { get; set; }
 = TimeSpan.FromSeconds(15);
 public TimeSpan PollingInterval { get; set; }
 = TimeSpan.FromSeconds(1);
 public TimeSpan StoreFailureGrace { get; set; }
 = TimeSpan.FromSeconds(30);
 public TimeSpan OwnerLeaseFencingMargin { get; set; }
 = TimeSpan.FromSeconds(5);
 public TimeSpan OwnerLeaseRenewTimeout { get; set; }
 = TimeSpan.FromSeconds(3);
 public TimeSpan RouteCacheMaxAge { get; set; }
 = TimeSpan.FromSeconds(15);
 public TimeSpan MessageFollowDuration { get; set; }
 = TimeSpan.FromSeconds(30);
 public TimeSpan SessionRelocationSealTimeout { get; set; }
 = TimeSpan.FromSeconds(3);
 public long RelocationPayloadChunkLimit { get; set; }
 = 256 * 1024;
 public long RelocationInFlightPayloadBudget { get; set; }
 = 16 * 1024 * 1024;
 public long RelocationNodeInFlightPayloadBudget { get; set; }
 = 0;
 public TimeSpan RelocationCutoverWaitTimeout { get; set; }
 = TimeSpan.FromSeconds(1);
}
```

The signature of the root's `AddLocationStore(...)`,
`AddRelocationStore(...)`, and `ConfigureLocations()` is owned by
[Topology Configuration](03-configuration-topology.en.md#2-registration-interface).
Exactly one Store is registered per role. Registering the same role twice
fails startup with `ZLinkConfigurationException` before socket bind.

Lease and polling options must be greater than 0. Every Location host
must satisfy the following relationship.

```text
OwnerLeaseRenewInterval + OwnerLeaseRenewTimeout
 < OwnerLeaseTtl - OwnerLeaseFencingMargin
```

`RouteCacheMaxAge` and `MessageFollowDuration` are at least 0. If both
are positive, cache age must be at least 5 seconds smaller than the
Message Follow duration. 0 turns that feature off.

`SessionRelocationSealTimeout` is a startup-only positive duration with a three-second
default. Zero, negative, infinite, or a value not representable as finite milliseconds is
a configuration error before socket bind.

`RelocationPayloadChunkLimit` is the maximum size in bytes of one encoded chunk of a
relocation payload, defaulting to 256 KiB. Setting it above the frame limit the
transport negotiated is a startup configuration error before socket bind.
`RelocationInFlightPayloadBudget` caps the sum of relocation chunk bytes concurrently
in flight per peer connection, defaulting to 16 MiB; `0` disables the budget.
`RelocationNodeInFlightPayloadBudget` applies the same accounting rule to the
node-wide sum and defaults to `0`, meaning not applied. `RelocationCutoverWaitTimeout`
is both the time the target waits for cutover and the time the source keeps its
boundary batch copy for retransmission, defaulting to one second. All four values are
startup-only, and negative values are a configuration error before socket bind.

## 3. Readiness And Operational Queries

```csharp
public sealed record ZLinkLocationRuntimeStatus(
 bool StoreHealthy,
 bool OwnerLeaseHealthy,
 DateTimeOffset? LastRefreshAt,
 DateTimeOffset? OwnerLeaseRenewedAt);

public enum ZLinkLocationTopologyState
{
 Discovered = 1,
 Connecting = 2,
 Ready = 3,
 Lost = 4,
 Error = 5,
 Stopped = 6
}

public sealed record ZLinkLocationTopologyFilter(
 string? MeshName = null,
 RoutingId? NodeRid = null,
 ZLinkLocationTopologyState? State = null);

public sealed record ZLinkLocationTopologyEntry(
 string MeshName,
 RoutingId NodeRid,
 string Endpoint,
 bool Draining,
 ZLinkLocationTopologyState State,
 DateTimeOffset UpdatedAt);

public sealed record ZLinkLocationServiceSummaryFilter(
 string? MeshName = null);

public sealed record ZLinkLocationServiceSummary(
 string MeshName,
 uint TotalCount,
 uint ReadyCount,
 uint ErrorCount,
 uint StoppedCount,
 DateTimeOffset LastUpdatedAt);

public enum ZLinkLocationObjectKind
{
 Actor = 0,
 UserSpot = 1,
 InstanceSpot = 2
}

public enum ZLinkLocationObjectState
{
 Creating = 0,
 Ready = 1,
 Unavailable = 2
}

public sealed record ZLinkLocationObjectEntry(
 string GlobalId,
 ulong ObjectGeneration,
 string MeshName,
 RoutingId NodeRid,
 ZLinkLocationObjectState State,
 string StableType);

public sealed record ZLinkLocationObjectFilter(
 ZLinkLocationObjectKind ObjectKind,
 string? StableType = null,
 string? MeshName = null);

public readonly record struct ZLinkPageRequest(
 int PageSize = 100,
 string? ContinuationToken = null);

public sealed record ZLinkLocationPage<T>(
 IReadOnlyList<T> Items,
 string? ContinuationToken);

public interface IZLinkLocationReadiness
{
 ValueTask<bool> IsPeerReadyAsync(
 string meshName,
 ZLinkLocationRole role,
 RoutingId? nodeRid = null,
 CancellationToken cancellationToken = default);
}

public interface IZLinkLocationRuntimeQuery
{
 ValueTask<ZLinkLocationRuntimeStatus> GetStatusAsync(
 CancellationToken cancellationToken = default);

 ValueTask<ZLinkLocationPage<ZLinkLocationTopologyEntry>> ListTopologyAsync(
 ZLinkLocationTopologyFilter filter,
 ZLinkPageRequest page = default,
 CancellationToken cancellationToken = default);

 ValueTask<ZLinkLocationPage<ZLinkLocationServiceSummary>>
 ListServiceSummariesAsync(
 ZLinkLocationServiceSummaryFilter filter,
 ZLinkPageRequest page = default,
 CancellationToken cancellationToken = default);

 ValueTask<ZLinkLocationObjectEntry?> FindActorLocationAsync(
 string actorId,
 CancellationToken cancellationToken = default);

 ValueTask<ZLinkLocationObjectEntry?> FindSpotLocationAsync(
 string spotId,
 CancellationToken cancellationToken = default);

 ValueTask<ZLinkLocationPage<ZLinkLocationObjectEntry>>
 ListObjectLocationsAsync(
 ZLinkLocationObjectFilter filter,
 ZLinkPageRequest page = default,
 CancellationToken cancellationToken = default);
}

public enum ZLinkLocationRole : ushort
{
 Invalid = 0,
 Spot = 2,
 Router = 3,
 Dealer = 4,
 Pub = 5,
 Sub = 6
}
```

An operational query only returns human-readable health/topology/service
summary and object location. It doesn't return Store key/version, owner lease generation,
descriptor payload, or protocol envelope. `NodeRid` is kept as the public
`RoutingId` since it's the actual transport routing identity.

Page size is `1..1000`, and the continuation token is an opaque value
issued by that query. The application doesn't interpret the token or use
it in a different query.

Direct lookup by Actor ID and Spot ID each queries one current object
location. Missing returns `null`; Creating returns a `Creating` entry;
Ready returns a `Ready` entry; and an unavailable current owner after
commit returns an `Unavailable` entry. Spot direct lookup treats User Spot
and Instance Spot under the same Spot-ID lookup contract. A list requires
`ObjectKind`, and takes `StableType` and `MeshName` as optional filters.
The encoded page is at most 4 MiB. A Store query failure is
`ZLinkFrameworkErrorKind.Unavailable` and does not return a partial page.

## 4. Host Maintenance

Host maintenance is owned by `IZLinkFrameworkRuntime.RelocateAsync(...)`
and `ShutdownAsync(...)`. `RelocateAsync(...)` moves as much workload as
possible to a different owner and completes in the `Relocated` state.
`PlannedMaintenance` only moves to the same application version as
source, and doesn't take a target version. `RollingUpdate` requires a
target version greater than source, and only moves to a node matching
that version exactly. Both modes restrict and select candidates in the
order version, maintenance wave, capability, capacity, placement weight.
If there's no target satisfying the requested condition, it waits until
the deadline and then completes with `Blocked/TargetUnavailable`. The
application can shut down the host with `ShutdownAsync(...)` after
confirming the result. Calling `ShutdownAsync(...)` directly from
`Serving` doesn't start a new relocation, and shuts down the host after
bounded cleanup. The signature and result are owned by
[Host Monitoring](10-topology-monitoring.en.md).
