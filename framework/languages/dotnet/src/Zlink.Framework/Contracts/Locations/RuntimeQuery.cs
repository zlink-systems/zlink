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
}
