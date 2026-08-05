namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Highest logical version this runtime has accepted per location key, shared
/// by every read surface (resolvers and the runtime query). A read whose
/// version is strictly older than the recorded one is a lagging replica view
/// and never counts as a success result. Entries persist until the store
/// confirms the row is gone — only a confirmed miss may forget a floor,
/// because a re-created incarnation legitimately restarts its version axes.
///
/// Per the location runtime contract the monotonic axes are the rows' own
/// spec fields: descriptors order by (lifecycle generation, descriptor
/// revision), spot rows by spot generation, and actor rows by
/// (actor generation, membership epoch).
/// </summary>
internal sealed class ZLinkObservedLocationGenerations
{
    private readonly ObservedDescriptors _meshNodes = new();
    private readonly Observed<ZLinkSpotLocationKey> _spots = new();
    private readonly Observed<ZLinkActorLocationKey> _actors = new();

    internal bool AcceptDescriptor(ZLinkMeshNodeDescriptor row) =>
        AcceptDescriptor(row, out _);

    internal bool AcceptDescriptor(
        ZLinkMeshNodeDescriptor row,
        out bool rejectedByOlderRevision)
    {
        var result = _meshNodes.Accept(
            new ZLinkMeshNodeDescriptorKey(row.MeshName, row.Rid),
            row.OwnerId,
            new ObservedVersion(row.LifecycleGeneration, row.DescriptorRevision));
        rejectedByOlderRevision =
            result == DescriptorAcceptance.RejectedByOlderRevision;
        return result == DescriptorAcceptance.Accepted;
    }

    internal void ObserveDescriptor(ZLinkMeshNodeDescriptor row) =>
        _meshNodes.Observe(
            new ZLinkMeshNodeDescriptorKey(row.MeshName, row.Rid),
            row.OwnerId,
            new ObservedVersion(row.LifecycleGeneration, row.DescriptorRevision));

    internal bool AcceptSpot(ZLinkResolvedSpotLocation row) =>
        _spots.Accept(
            new ZLinkSpotLocationKey(row.SpotId),
            new ObservedVersion(row.SpotGeneration, 0));

    internal void ObserveSpot(ZLinkResolvedSpotLocation row) =>
        _spots.Observe(
            new ZLinkSpotLocationKey(row.SpotId),
            new ObservedVersion(row.SpotGeneration, 0));

    // Membership epoch is the major axis: it increases monotonically across
    // joins and cross-node transfers, while the actor generation is a
    // per-node counter that legitimately restarts when the actor moves to
    // another node (core next_actor_generation) — generation-major ordering
    // would reject the post-transfer row as a lagging replica forever.
    internal bool AcceptActor(ZLinkResolvedActorLocation row) =>
        _actors.Accept(
            new ZLinkActorLocationKey(row.ActorId),
            new ObservedVersion(
                row.MembershipEpoch,
                row.ActorRef.ObjectGeneration));

    internal void ObserveActor(ZLinkResolvedActorLocation row) =>
        _actors.Observe(
            new ZLinkActorLocationKey(row.ActorId),
            new ObservedVersion(
                row.MembershipEpoch,
                row.ActorRef.ObjectGeneration));

    /// <summary>A completed store listing is a confirmed view of the mesh:
    /// a floor held for a key the store no longer returns belongs to a
    /// removed row, and keeping it would hide the successor row a restarted
    /// node publishes with a fresh (restarted) lifecycle generation.</summary>
    internal void ReconcileDescriptors(
        string meshName, IReadOnlyList<ZLinkMeshNodeDescriptor> rows)
    {
        var present = new HashSet<ZLinkMeshNodeDescriptorKey>(rows.Count);
        foreach (var row in rows)
            present.Add(new ZLinkMeshNodeDescriptorKey(row.MeshName, row.Rid));
        _meshNodes.ForgetWhere(key =>
            string.Equals(key.MeshName, meshName, StringComparison.Ordinal)
            && !present.Contains(key));
    }

    /// <summary>A confirmed store miss ends the identity's observed lifecycle:
    /// the row was removed, so a re-created incarnation legitimately restarts
    /// its generation axes and must not be rejected as a lagging replica.</summary>
    internal void ForgetActor(ZLinkActorLocationKey key) => _actors.Forget(key);

    /// <summary>See <see cref="ForgetActor"/>; the same lifecycle-restart rule
    /// applies to re-created spots.</summary>
    internal void ForgetSpot(ZLinkSpotLocationKey key) => _spots.Forget(key);

    private readonly record struct ObservedVersion(ulong Major, ulong Minor)
        : IComparable<ObservedVersion>
    {
        public int CompareTo(ObservedVersion other)
        {
            var major = Major.CompareTo(other.Major);
            return major != 0 ? major : Minor.CompareTo(other.Minor);
        }
    }

    private enum DescriptorAcceptance
    {
        Accepted,
        RejectedByOlderRevision,
        RejectedRetiredOwner
    }

    /// <summary>
    /// MeshNode generation is monotonic only inside one descriptor owner
    /// incarnation. A successful store takeover replaces OwnerId atomically
    /// and the successor process may have a lower wall-clock-derived Core
    /// generation. Remember retired owners so a lagging view cannot restore
    /// the predecessor after the successor has been observed.
    /// </summary>
    private sealed class ObservedDescriptors
    {
        private readonly object _gate = new();
        private readonly Dictionary<ZLinkMeshNodeDescriptorKey, DescriptorObservation> _observed = [];

        internal DescriptorAcceptance Accept(
            ZLinkMeshNodeDescriptorKey key,
            string ownerId,
            ObservedVersion version)
        {
            lock (_gate)
            {
                if (!_observed.TryGetValue(key, out var observed))
                {
                    _observed[key] = new DescriptorObservation(ownerId, version, []);
                    return DescriptorAcceptance.Accepted;
                }

                if (string.Equals(observed.OwnerId, ownerId, StringComparison.Ordinal))
                {
                    if (version.CompareTo(observed.Version) < 0)
                        return DescriptorAcceptance.RejectedByOlderRevision;
                    _observed[key] = observed with { Version = version };
                    return DescriptorAcceptance.Accepted;
                }

                if (observed.RetiredOwners.Contains(ownerId))
                    return DescriptorAcceptance.RejectedRetiredOwner;

                observed.RetiredOwners.Add(observed.OwnerId);
                _observed[key] = new DescriptorObservation(
                    ownerId, version, observed.RetiredOwners);
                return DescriptorAcceptance.Accepted;
            }
        }

        internal void Observe(
            ZLinkMeshNodeDescriptorKey key,
            string ownerId,
            ObservedVersion version) => Accept(key, ownerId, version);

        internal void ForgetWhere(Func<ZLinkMeshNodeDescriptorKey, bool> predicate)
        {
            lock (_gate)
            {
                List<ZLinkMeshNodeDescriptorKey>? stale = null;
                foreach (var key in _observed.Keys)
                    if (predicate(key))
                        (stale ??= []).Add(key);
                if (stale is null) return;
                foreach (var key in stale)
                    _observed.Remove(key);
            }
        }

        private sealed record DescriptorObservation(
            string OwnerId,
            ObservedVersion Version,
            HashSet<string> RetiredOwners);
    }

    private sealed class Observed<TKey>
        where TKey : notnull
    {
        private readonly object _gate = new();
        private readonly Dictionary<TKey, ObservedVersion> _versions = [];

        internal void Forget(TKey key)
        {
            lock (_gate)
            {
                _versions.Remove(key);
            }
        }

        internal void ForgetWhere(Func<TKey, bool> predicate)
        {
            lock (_gate)
            {
                List<TKey>? stale = null;
                foreach (var key in _versions.Keys)
                    if (predicate(key))
                        (stale ??= []).Add(key);
                if (stale is null) return;
                foreach (var key in stale)
                    _versions.Remove(key);
            }
        }

        internal bool Accept(TKey key, ObservedVersion version)
        {
            lock (_gate)
            {
                if (_versions.TryGetValue(key, out var observed)
                    && version.CompareTo(observed) < 0)
                {
                    return false;
                }

                _versions[key] = version;
                return true;
            }
        }

        internal void Observe(TKey key, ObservedVersion version)
        {
            lock (_gate)
            {
                if (!_versions.TryGetValue(key, out var observed)
                    || version.CompareTo(observed) > 0)
                    _versions[key] = version;
            }
        }
    }
}
