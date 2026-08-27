using Zlink.Framework.Contracts.Locations;

namespace Zlink.Framework.Runtime.Service;

/// <summary>
/// Keeps placement selection aligned with the RouteMesh admission view.
/// A live location row is not enough for a NodeRid-targeted operation: the
/// source must have an admitted peer for the same lifecycle generation.
/// </summary>
internal static class ZLinkMeshNodeTargetAvailability
{
    // A submit can discover that an otherwise admitted peer has no usable
    // transport. Keep that result local to the selecting operation: it is not
    // a Mesh admission transition. A later peer-state change produces a new
    // epoch and makes the target eligible again.
    internal readonly record struct PeerEpoch(
        RoutingId RoutingId,
        ulong LifecycleGeneration,
        ulong LastChangedMs);

    internal static IReadOnlyList<ZLinkMeshNodeDescriptor> FilterAdmitted(
        RoutingId localRid,
        IReadOnlyList<ZLinkMeshNodeDescriptor> candidates,
        IReadOnlyList<MeshNodePeer> peers,
        IReadOnlySet<PeerEpoch>? unavailablePeerEpochs = null)
    {
        var admitted = peers
            .Where(static peer => peer.State == MeshPeerState.Admitted)
            .ToDictionary(
                static peer => peer.RoutingId,
                static peer => new PeerEpoch(
                    peer.RoutingId,
                    peer.LifecycleGeneration,
                    peer.LastChangedMs));

        return candidates
            .Where(candidate => candidate.Rid == localRid
                || admitted.TryGetValue(candidate.Rid, out var epoch)
                   && epoch.LifecycleGeneration == candidate.LifecycleGeneration
                   && (unavailablePeerEpochs is null
                       || !unavailablePeerEpochs.Contains(epoch)))
            .ToArray();
    }

    internal static bool TryGetAdmittedPeerEpoch(
        RoutingId peerRid,
        ulong lifecycleGeneration,
        IReadOnlyList<MeshNodePeer> peers,
        out PeerEpoch epoch)
    {
        var peer = peers.FirstOrDefault(candidate =>
            candidate.State == MeshPeerState.Admitted
            && candidate.RoutingId == peerRid
            && candidate.LifecycleGeneration == lifecycleGeneration);
        if (peer is null)
        {
            epoch = default;
            return false;
        }

        epoch = new PeerEpoch(
            peer.RoutingId,
            peer.LifecycleGeneration,
            peer.LastChangedMs);
        return true;
    }
}
