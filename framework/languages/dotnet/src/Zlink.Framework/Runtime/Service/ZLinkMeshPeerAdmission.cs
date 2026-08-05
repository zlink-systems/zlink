using Systems.Zlink;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

/// <summary>
/// Selects the configured peer that owns an admission and resolves duplicate
/// connections before the mesh node mutates its peer indexes.
/// </summary>
/// <remarks>
/// The matcher does not perform socket or catalog mutations. Keeping identity
/// matching here makes the admission order explicit and prevents a transport
/// callback from choosing an unadmitted connection by dictionary iteration
/// order.
/// </remarks>
internal sealed class ZLinkMeshPeerAdmission
{
    internal ZLinkMeshPeer? FindForAdmission(
        IReadOnlyDictionary<RoutingId, ZLinkMeshPeer> peersByRid,
        IEnumerable<ZLinkMeshPeer> peersByIntent,
        RoutingId sourceRid,
        ServiceWireConstants.Command command,
        string advertisedEndpoint)
    {
        ArgumentNullException.ThrowIfNull(peersByRid);
        ArgumentNullException.ThrowIfNull(peersByIntent);
        ArgumentNullException.ThrowIfNull(advertisedEndpoint);

        if (peersByRid.TryGetValue(sourceRid, out var exact)
            && (command == ServiceWireConstants.Command.Update
                || command == ServiceWireConstants.Command.Hello
                    && exact.Direction == ZLinkServiceConnectionDirection.Inbound
                || command == ServiceWireConstants.Command.Admit
                    && exact.Direction == ZLinkServiceConnectionDirection.Outbound))
            return exact;

        var peers = peersByIntent.ToArray();
        if (command == ServiceWireConstants.Command.Hello)
            return peers
                .Where(peer =>
                    peer.Direction == ZLinkServiceConnectionDirection.Inbound
                    && !peer.Admitted
                    && (peer.State == MeshPeerState.NotRequired
                        || peer.State == MeshPeerState.Connecting)
                    && peer.RoutingId == sourceRid)
                .OrderBy(static peer => peer.Discriminator, StringComparer.Ordinal)
                .FirstOrDefault();

        var candidates = peers
            .Where(peer =>
                peer.Direction == ZLinkServiceConnectionDirection.Outbound
                && !peer.Admitted)
            .OrderBy(static peer => peer.Discriminator, StringComparer.Ordinal)
            .ToArray();
        var identityMatch = candidates.FirstOrDefault(peer =>
            peer.ExpectedRid == sourceRid
            || peer.PhysicalRoutingId == sourceRid);
        if (identityMatch is not null)
            return identityMatch;

        // When the caller did not configure an expected RID, multiple
        // outbound intents are distinguished by the endpoint echoed in the
        // admission descriptor. Selecting the first unadmitted intent would
        // attach two different peers to the same physical pipe.
        var endpointMatch = candidates.FirstOrDefault(peer =>
            peer.ExpectedRid is null
            && string.Equals(
                peer.Endpoint,
                advertisedEndpoint,
                StringComparison.Ordinal));
        if (endpointMatch is not null)
            return endpointMatch;

        var unknownIdentity = candidates
            .Where(static peer => peer.ExpectedRid is null)
            .Take(2)
            .ToArray();
        return unknownIdentity.Length == 1 ? unknownIdentity[0] : null;
    }

    internal ZLinkMeshPeer? FindDuplicate(
        IReadOnlyDictionary<RoutingId, ZLinkMeshPeer> peersByRid,
        IEnumerable<ZLinkMeshPeer> peersByIntent,
        RoutingId sourceRid,
        ZLinkMeshPeer candidate)
    {
        ArgumentNullException.ThrowIfNull(peersByRid);
        ArgumentNullException.ThrowIfNull(peersByIntent);
        ArgumentNullException.ThrowIfNull(candidate);

        if (peersByRid.TryGetValue(sourceRid, out var admitted)
            && !ReferenceEquals(admitted, candidate))
            return admitted;
        return peersByIntent
            .Where(peer =>
                !ReferenceEquals(peer, candidate)
                && (peer.RoutingId == sourceRid
                    || peer.ExpectedRid == sourceRid
                    || peer.PhysicalRoutingId == sourceRid
                    || peer.ExpectedRid is null
                        && !string.IsNullOrWhiteSpace(candidate.Endpoint)
                        && string.Equals(
                            peer.Endpoint,
                            candidate.Endpoint,
                            StringComparison.Ordinal)))
            .OrderBy(static peer => peer.Discriminator, StringComparer.Ordinal)
            .FirstOrDefault();
    }

    internal static ZLinkMeshPeer? FindNotRequiredDuplicate(
        IEnumerable<ZLinkMeshPeer> peersByIntent,
        ZLinkMeshPeer candidate,
        RoutingId sourceRid)
    {
        ArgumentNullException.ThrowIfNull(peersByIntent);
        ArgumentNullException.ThrowIfNull(candidate);
        return peersByIntent.FirstOrDefault(existingPeer =>
            !ReferenceEquals(existingPeer, candidate)
            && existingPeer.State == MeshPeerState.NotRequired
            && existingPeer.RoutingId == sourceRid);
    }
}

internal readonly record struct ZLinkMeshPeerExpectation(
    string Endpoint,
    string SecurityIdentity,
    ulong LifecycleGeneration);
