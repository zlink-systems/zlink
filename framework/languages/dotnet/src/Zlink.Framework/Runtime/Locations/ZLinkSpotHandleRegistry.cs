using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Weakly tracks the resolved spot handles handed out to messaging callers
/// so location events can update or invalidate their snapshots in place.
/// Spot handles order updates by the row's spot generation; actor handles
/// order by membership epoch, the axis that changes on every membership
/// move within one actor generation.
/// </summary>
internal sealed class ZLinkSpotHandleRegistry
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<ZLinkActorLocationKey, List<WeakReference<ZLinkResolvedSpotHandle>>> _actors = [];
    private readonly Dictionary<ZLinkSpotLocationKey, List<WeakReference<ZLinkResolvedSpotHandle>>> _spots = [];

    internal ValueTask RegisterSpotAsync(
        ZLinkSpotLocationKey key,
        ZLinkResolvedSpotHandle handle) =>
        _lane.RunAsync(() => Register(_spots, key, handle));

    internal ValueTask RegisterActorAsync(
        ZLinkActorLocationKey key,
        ZLinkResolvedSpotHandle handle) =>
        _lane.RunAsync(() => Register(_actors, key, handle));

    internal async ValueTask UpdateSpotAsync(ZLinkResolvedSpotLocation row)
    {
        var snapshot = new ZLinkSpotHandleSnapshot(
            row.MeshName,
            row.OwnerNodeRid,
            row.SpotId,
            row.SpotGeneration,
            row.SpotKind,
            row.AuthorityOwnerGeneration,
            row.OwnerNodeGeneration,
            checked((ulong)row.LeaseGeneration));
        var handles = await _lane.RunAsync(() => Apply(
                _spots,
                new ZLinkSpotLocationKey(row.SpotId)))
            .ConfigureAwait(false);
        foreach (var handle in handles)
            handle.Update(snapshot, row.SpotGeneration);
    }

    internal async ValueTask RemoveSpotAsync(ZLinkSpotLocationKey key, ulong spotGeneration)
    {
        var handles = await _lane.RunAsync(() => Apply(_spots, key))
            .ConfigureAwait(false);
        foreach (var handle in handles)
            handle.Invalidate(spotGeneration);
    }

    internal async ValueTask UpdateActorAsync(ZLinkResolvedActorLocation row)
    {
        var snapshot = ToSnapshot(row);
        var handles = await _lane.RunAsync(() => Apply(
                _actors,
                new ZLinkActorLocationKey(row.ActorId)))
            .ConfigureAwait(false);
        foreach (var handle in handles)
            handle.Update(snapshot, row.MembershipEpoch);
    }

    private ZLinkSpotHandleSnapshot ToSnapshot(ZLinkResolvedActorLocation row)
        => row.SpotKind == ZLinkSpotKind.Entry || string.IsNullOrEmpty(row.SpotId)
            ? new ZLinkSpotHandleSnapshot(
                row.MeshName,
                row.OwnerNodeRid,
                row.SpotId,
                row.SpotGeneration,
                ZLinkSpotKind.Entry,
                row.AuthorityOwnerGeneration,
                row.OwnerNodeGeneration,
                checked((ulong)row.LeaseGeneration))
            : new ZLinkSpotHandleSnapshot(
                row.MeshName,
                row.OwnerNodeRid,
                row.SpotId,
                row.SpotGeneration,
                ZLinkSpotKind.User,
                row.AuthorityOwnerGeneration,
                row.OwnerNodeGeneration,
                checked((ulong)row.LeaseGeneration));

    internal async ValueTask RemoveActorAsync(ZLinkActorLocationKey key)
    {
        var handles = await _lane.RunAsync(() => Apply(_actors, key))
            .ConfigureAwait(false);
        foreach (var handle in handles)
            handle.InvalidateCurrent();
    }

    internal ValueTask<IReadOnlyList<ZLinkResolvedSpotHandle>> SnapshotLiveHandlesAsync() =>
        _lane.RunAsync<IReadOnlyList<ZLinkResolvedSpotHandle>>(() =>
        {
            var live = new List<ZLinkResolvedSpotHandle>();
            Collect(_spots, live);
            Collect(_actors, live);
            return live;
        });

    private static void Collect<TKey>(
        Dictionary<TKey, List<WeakReference<ZLinkResolvedSpotHandle>>> handles,
        List<ZLinkResolvedSpotHandle> live)
        where TKey : notnull
    {
        foreach (var entries in handles.Values)
            foreach (var entry in entries)
                if (entry.TryGetTarget(out var handle))
                    live.Add(handle);
    }

    private void Register<TKey>(
        Dictionary<TKey, List<WeakReference<ZLinkResolvedSpotHandle>>> handles,
        TKey key,
        ZLinkResolvedSpotHandle handle)
        where TKey : notnull
    {
        if (!handles.TryGetValue(key, out var entries))
        {
            entries = [];
            handles.Add(key, entries);
        }
        entries.RemoveAll(static entry => !entry.TryGetTarget(out _));
        entries.Add(new WeakReference<ZLinkResolvedSpotHandle>(handle));
    }

    private static IReadOnlyList<ZLinkResolvedSpotHandle> Apply<TKey>(
        Dictionary<TKey, List<WeakReference<ZLinkResolvedSpotHandle>>> handles,
        TKey key)
        where TKey : notnull
    {
        if (!handles.TryGetValue(key, out var entries))
            return Array.Empty<ZLinkResolvedSpotHandle>();
        var live = new List<ZLinkResolvedSpotHandle>();
        entries.RemoveAll(entry =>
        {
            if (!entry.TryGetTarget(out var handle)) return true;
            live.Add(handle);
            return false;
        });
        if (entries.Count == 0) handles.Remove(key);
        return live;
    }
}
