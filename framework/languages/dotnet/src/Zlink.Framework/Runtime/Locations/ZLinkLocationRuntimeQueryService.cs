namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Operational read surface. Every query reads the registered store
/// directly — no cache is consulted or written, which is why this surface
/// takes no freshness. Rows whose owner lease expired and rows older than a
/// version this runtime already observed are filtered out of success
/// results everywhere. Spot and Actor rows are resolve-only store records
/// (06-location-store §5), so topology and summaries project MeshNode
/// descriptors only.
/// </summary>
internal sealed class ZLinkLocationRuntimeQueryService :
    IZLinkLocationRuntimeQuery,
    IZLinkLocationDescriptorQuery
{
    private readonly ZLinkLocationOptions _options;
    private readonly IZLinkLocationRepository _meshNodeStore;
    private readonly IReadOnlyCollection<string> _registeredMeshNames;
    private readonly ZLinkOwnerLeaseTracker _leaseTracker;
    private readonly ZLinkLocationRuntime _runtime;
    private readonly ZLinkObservedLocationGenerations _observed;
    private readonly ZLinkLiveLocationRows _liveRows;
    private readonly ZLinkLocationStoreHealth? _storeHealth;

    internal ZLinkLocationRuntimeQueryService(
        ZLinkLocationOptions options,
        IZLinkLocationRepository meshNodeStore,
        IReadOnlyCollection<string> registeredMeshNames,
        ZLinkOwnerLeaseTracker leaseTracker,
        ZLinkLocationRuntime runtime,
        ZLinkObservedLocationGenerations observed,
        ZLinkLocationStoreHealth? storeHealth = null)
    {
        _options = options;
        _meshNodeStore = meshNodeStore;
        _registeredMeshNames = registeredMeshNames;
        _leaseTracker = leaseTracker;
        _runtime = runtime;
        _observed = observed;
        _storeHealth = storeHealth;
        _liveRows = new ZLinkLiveLocationRows(leaseTracker);
    }

    public ValueTask<ZLinkLocationRuntimeStatus> GetStatusAsync(
        CancellationToken cancellationToken = default)
    {
        var health = _runtime.GetHealthSnapshot();
        var store = _storeHealth?.GetSnapshot();
        var lastRefreshAt = store?.LastSuccessAt;
        if (health.RenewedAt is { } renewedAt
            && (lastRefreshAt is null || renewedAt > lastRefreshAt))
        {
            lastRefreshAt = renewedAt;
        }

        return ValueTask.FromResult(new ZLinkLocationRuntimeStatus(
            StoreHealthy: health.LastError is null && (store?.Healthy ?? true),
            LastRefreshAt: lastRefreshAt,
            OwnerLeaseHealthy: health.Healthy,
            OwnerLeaseRenewedAt: health.RenewedAt));
    }

    public async ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
        ListMeshNodeDescriptorsAsync(
        string meshName,
        ZLinkPageRequest page = default,
        CancellationToken cancellationToken = default)
    {
        var rows = await ListAcceptedDescriptorsAsync(meshName, cancellationToken)
            .ConfigureAwait(false);
        var live = await _liveRows.FilterAsync(
                rows,
                static row => row.OwnerId,
                static _ => true,
                cancellationToken)
            .ConfigureAwait(false);
        return PageInMemory(live, Normalize(page));
    }

    public async ValueTask<ZLinkLocationPage<ZLinkLocationTopologyEntry>> ListTopologyAsync(
        ZLinkLocationTopologyFilter filter,
        ZLinkPageRequest page = default,
        CancellationToken cancellationToken = default)
    {
        // The projection is built by the framework from descriptors plus
        // liveness; stores never decide topology meaning. Until a
        // descriptor's owner lease expires it reports Ready, afterwards
        // Lost.
        var entries = new List<ZLinkLocationTopologyEntry>();
        foreach (var meshName in MeshNamesOf(filter.MeshName))
        {
            var rows = await ListAcceptedDescriptorsAsync(meshName, cancellationToken)
                .ConfigureAwait(false);
            foreach (var row in rows)
            {
                var live = await _leaseTracker.IsOwnerLiveAsync(row.OwnerId, cancellationToken)
                    .ConfigureAwait(false);
                //  Spec 13 §365는 relocate가 unit seal을 마치면 source가 draining으로
                //  넘어간다고 정하고, §409는 draining node가 새 placement target이
                //  되지 않는다고 정한다. Host lifecycle의 `Draining`만 보면 relocate로
                //  빠지는 node가 계속 accepting으로 보인다.
                var entry = new ZLinkLocationTopologyEntry(
                    row.MeshName, row.Rid, row.Endpoint,
                    row.State is ZLinkFrameworkRuntimeState.Draining
                        or ZLinkFrameworkRuntimeState.Relocating
                        or ZLinkFrameworkRuntimeState.Relocated,
                    live ? ZLinkLocationTopologyState.Ready : ZLinkLocationTopologyState.Lost,
                    row.UpdatedAt);
                if (Matches(entry, filter)) entries.Add(entry);
            }
        }

        return PageInMemory(entries, Normalize(page));
    }

    public async ValueTask<ZLinkLocationPage<ZLinkLocationServiceSummary>>
        ListServiceSummariesAsync(
        ZLinkLocationServiceSummaryFilter filter,
        ZLinkPageRequest page = default,
        CancellationToken cancellationToken = default)
    {
        var summaries = new List<ZLinkLocationServiceSummary>();
        foreach (var meshName in MeshNamesOf(filter.MeshName))
        {
            var rows = await ListAcceptedDescriptorsAsync(meshName, cancellationToken)
                .ConfigureAwait(false);
            if (rows.Count == 0) continue;

            var accumulator = new Accumulator();
            foreach (var row in rows)
            {
                accumulator.Total++;
                if (await _leaseTracker.IsOwnerLiveAsync(row.OwnerId, cancellationToken)
                        .ConfigureAwait(false))
                {
                    accumulator.Ready++;
                }
                else
                {
                    accumulator.Stopped++;
                }

                if (row.UpdatedAt > accumulator.LastUpdatedAt)
                {
                    accumulator.LastUpdatedAt = row.UpdatedAt;
                }
            }

            summaries.Add(new ZLinkLocationServiceSummary(
                meshName,
                accumulator.Total, accumulator.Ready, 0, accumulator.Stopped,
                accumulator.LastUpdatedAt));
        }

        return PageInMemory(summaries, Normalize(page));
    }

    private IEnumerable<string> MeshNamesOf(string? meshName) =>
        meshName is not null
            ? [meshName]
            : _registeredMeshNames;

    private static ZLinkPageRequest Normalize(ZLinkPageRequest page) =>
        ZLinkPageRequestPolicy.Normalize(page);

    private async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListAcceptedDescriptorsAsync(
        string meshName,
        CancellationToken cancellationToken)
    {
        var rows = await ZLinkLocationStoreRead.ExecuteAsync(
            _storeHealth,
            "mesh-node-query-read",
            cancellationToken,
            storeToken => _meshNodeStore.ListAllMeshNodesAsync(meshName, storeToken)).ConfigureAwait(false);
        _observed.ReconcileDescriptors(meshName, rows);
        return rows.Where(_observed.AcceptDescriptor).ToArray();
    }

    private static bool Matches(
        ZLinkLocationTopologyEntry entry,
        ZLinkLocationTopologyFilter filter) =>
        (filter.MeshName is null
            || string.Equals(entry.MeshName, filter.MeshName, StringComparison.Ordinal))
        && (filter.NodeRid is null || entry.NodeRid.Equals(filter.NodeRid.Value))
        && (filter.State is null || entry.State == filter.State);

    private static ZLinkLocationPage<T> PageInMemory<T>(
        IReadOnlyList<T> entries,
        ZLinkPageRequest page)
    {
        var offset = 0;
        if (page.ContinuationToken is { } token && int.TryParse(token, out var parsed))
        {
            offset = parsed;
        }

        var items = entries.Skip(offset).Take(page.PageSize).ToArray();
        var nextOffset = offset + items.Length;
        return new ZLinkLocationPage<T>(
            items,
            nextOffset < entries.Count ? nextOffset.ToString() : null);
    }

    private sealed class Accumulator
    {
        public uint Total;
        public uint Ready;
        public uint Stopped;
        public DateTimeOffset LastUpdatedAt;
    }
}
