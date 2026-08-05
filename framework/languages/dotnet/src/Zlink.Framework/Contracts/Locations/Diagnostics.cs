namespace Zlink.Framework.Contracts.Locations;

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

/// <summary>
/// One MeshNode descriptor projected with liveness. Spot and Actor rows are
/// resolve-only store records and never enumerate into topology.
/// </summary>
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
