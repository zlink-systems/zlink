using Zlink.Framework.Contracts.Locations;

namespace Zlink.Framework.Runtime.Service;

/// <summary>
/// Keeps placement selection aligned with the RouteMesh admission view.
/// A live location row is not enough for a NodeRid-targeted operation: the
/// source must have an admitted peer for the same lifecycle generation.
/// </summary>
internal static class ZLinkMeshNodeTargetAvailability
{
    internal static IReadOnlyList<ZLinkMeshNodeDescriptor> FilterAdmitted(
        RoutingId localRid,
        IReadOnlyList<ZLinkMeshNodeDescriptor> candidates,
        IReadOnlyList<MeshNodePeer> peers)
    {
        var admitted = peers
            .Where(static peer => peer.State == MeshPeerState.Admitted)
            .ToDictionary(
                static peer => peer.RoutingId,
                static peer => peer.LifecycleGeneration);

        return candidates
            .Where(candidate => candidate.Rid == localRid
                || admitted.TryGetValue(candidate.Rid, out var generation)
                   && generation == candidate.LifecycleGeneration)
            .ToArray();
    }
}
