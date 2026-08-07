namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// Operational read surface for tools and self-checks. Each query observes
/// the current location state. List methods return live rows only; topology
/// and summary queries also identify stale observations.
/// </summary>
public interface IZLinkLocationRuntimeQuery
{
    ValueTask<ZLinkLocationRuntimeStatus> GetStatusAsync(
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationPage<ZLinkLocationTopologyEntry>> ListTopologyAsync(
        ZLinkLocationTopologyFilter filter,
        ZLinkPageRequest page = default,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationPage<ZLinkLocationServiceSummary>> ListServiceSummariesAsync(
        ZLinkLocationServiceSummaryFilter filter,
        ZLinkPageRequest page = default,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationObjectEntry?> FindActorLocationAsync(
        string actorId,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationObjectEntry?> FindSpotLocationAsync(
        string spotId,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationPage<ZLinkLocationObjectEntry>> ListObjectLocationsAsync(
        ZLinkLocationObjectFilter filter,
        ZLinkPageRequest page = default,
        CancellationToken cancellationToken = default);
}

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
