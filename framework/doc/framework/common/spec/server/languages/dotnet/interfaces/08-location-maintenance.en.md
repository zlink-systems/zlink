# .NET Location Configuration And Operations Public Interface

[.NET exact interface table of contents](README.en.md) · [Location Runtime](../../../../21-location-runtime.en.md) ·
[Provider SPI](08-authority-relocation.ko.md) · [Host Monitoring](10-topology-monitoring.en.md)

## 1. Scope

This document only defines the Location configuration, readiness, and
operational queries the application uses. The technical primitives the
Store provider implements are owned by the
[provider SPI](08-authority-relocation.ko.md).

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
    public int MaxActiveOutboundRelocations { get; set; } = 64;
    public int MaxActiveInboundRelocations { get; set; } = 64;
    public int MaxConcurrentRelocationCaptures { get; set; } = 8;
    public int MaxConcurrentRelocationRestores { get; set; } = 8;
    public long MaxRelocationPayloadInFlightBytes { get; set; }
        = 268_435_456;
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

All five relocation limit options are positive. The default cap for
active outbound/inbound units is 64 each, the default cap for
Capture/Restore callbacks is 8 each, and the default cap for
process-wide encoded payload in-flight is 268,435,456 bytes. A runtime
change only applies to new relocation admission.

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
summary. It doesn't return Store key/version, owner lease generation,
descriptor payload, or protocol envelope. `NodeRid` is kept as the public
`RoutingId` since it's the actual transport routing identity.

Page size is `1..1000`, and the continuation token is an opaque value
issued by that query. The application doesn't interpret the token or use
it in a different query.

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
bounded cleanup. The exact signature and result are owned by
[Host Monitoring](10-topology-monitoring.en.md).
