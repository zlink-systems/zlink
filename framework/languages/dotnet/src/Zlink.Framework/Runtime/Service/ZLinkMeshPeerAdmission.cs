using Systems.Zlink;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

/// <summary>
/// Selects the logical peer that owns an admission and resolves duplicate
/// logical peer objects before the mesh node mutates its peer indexes.
/// </summary>
/// <remarks>
/// The matcher does not perform socket or catalog mutations. Core selects the
/// physical route of each RID (Core ROUTER §10.1); the matcher only binds a
/// validated handshake to the configured intent it identifies by RID or by the
/// advertised endpoint, so a transport callback never chooses a peer.
/// </remarks>
internal sealed class ZLinkMeshPeerAdmission
{
    internal ZLinkMeshPeer? FindForAdmission(
        IReadOnlyDictionary<RoutingId, ZLinkMeshPeer> peersByRid,
        IEnumerable<ZLinkMeshPeer> peersByIntent,
        RoutingId sourceRid,
        ServiceWireConstants.Command command,
        string advertisedEndpoint
    )
    {
        ArgumentNullException.ThrowIfNull(peersByRid);
        ArgumentNullException.ThrowIfNull(peersByIntent);
        ArgumentNullException.ThrowIfNull(advertisedEndpoint);

        if (
            peersByRid.TryGetValue(sourceRid, out var exact)
            && (
                exact.Admitted
                || command == ServiceWireConstants.Command.Update
                || command == ServiceWireConstants.Command.Hello
                    && exact.Direction == ZLinkServiceConnectionDirection.Inbound
                || command == ServiceWireConstants.Command.Admit
                    && exact.Direction == ZLinkServiceConnectionDirection.Outbound
            )
        )
            return exact;

        var peers = peersByIntent.ToArray();
        if (command == ServiceWireConstants.Command.Hello)
        {
            var inbound = peers
                .Where(peer =>
                    peer.Direction == ZLinkServiceConnectionDirection.Inbound
                    && !peer.Admitted
                    && (
                        peer.State == MeshPeerState.NotRequired
                        || peer.State == MeshPeerState.Connecting
                    )
                    && peer.RoutingId == sourceRid
                )
                .OrderBy(static peer => peer.Discriminator, StringComparer.Ordinal)
                .FirstOrDefault();
            if (inbound is not null)
                return inbound;
            // Either side may send Hello on the selected route. Without a
            // pending inbound peer, the Hello belongs to the configured
            // outbound intent that the RID or the advertised endpoint names.
        }

        var candidates = peers
            .Where(peer =>
                peer.Direction == ZLinkServiceConnectionDirection.Outbound && !peer.Admitted
            )
            .OrderBy(static peer => peer.Discriminator, StringComparer.Ordinal)
            .ToArray();
        var identityMatch = candidates.FirstOrDefault(peer =>
            peer.ExpectedRid == sourceRid || peer.PhysicalRoutingId == sourceRid
        );
        if (identityMatch is not null)
            return identityMatch;

        // An intent without an expected RID learns its RID from the endpoint
        // echoed in the admission descriptor. A handshake that names no
        // configured endpoint belongs to no intent; binding it to an intent
        // would only reject that intent's own later handshake.
        return candidates.FirstOrDefault(peer =>
            peer.ExpectedRid is null
            && string.Equals(peer.Endpoint, advertisedEndpoint, StringComparison.Ordinal)
        );
    }

    internal ZLinkMeshPeer? FindDuplicate(
        IReadOnlyDictionary<RoutingId, ZLinkMeshPeer> peersByRid,
        IEnumerable<ZLinkMeshPeer> peersByIntent,
        RoutingId sourceRid,
        ZLinkMeshPeer candidate
    )
    {
        ArgumentNullException.ThrowIfNull(peersByRid);
        ArgumentNullException.ThrowIfNull(peersByIntent);
        ArgumentNullException.ThrowIfNull(candidate);

        if (
            peersByRid.TryGetValue(sourceRid, out var admitted)
            && !ReferenceEquals(admitted, candidate)
        )
            return admitted;
        return peersByIntent
            .Where(peer =>
                !ReferenceEquals(peer, candidate)
                && (
                    peer.RoutingId == sourceRid
                    || peer.ExpectedRid == sourceRid
                    || peer.PhysicalRoutingId == sourceRid
                    || peer.ExpectedRid is null
                        && !string.IsNullOrWhiteSpace(candidate.Endpoint)
                        && string.Equals(
                            peer.Endpoint,
                            candidate.Endpoint,
                            StringComparison.Ordinal
                        )
                )
            )
            .OrderBy(static peer => peer.Discriminator, StringComparer.Ordinal)
            .FirstOrDefault();
    }

    internal static ZLinkMeshPeer? FindNotRequiredDuplicate(
        IEnumerable<ZLinkMeshPeer> peersByIntent,
        ZLinkMeshPeer candidate,
        RoutingId sourceRid
    )
    {
        ArgumentNullException.ThrowIfNull(peersByIntent);
        ArgumentNullException.ThrowIfNull(candidate);
        var peers = peersByIntent
            .Where(existingPeer =>
                !ReferenceEquals(existingPeer, candidate)
                && existingPeer.State == MeshPeerState.NotRequired
                && (
                    existingPeer.RoutingId == sourceRid
                    || existingPeer.ExpectedRid == sourceRid
                    || existingPeer.PhysicalRoutingId == sourceRid
                )
            )
            .ToArray();
        if (peers.Length != 0)
            return peers[0];

        // A client-to-client connection can expose a transport routing id for
        // the inbound half while the configured outbound intent retains the
        // logical peer id. When both halves describe the same endpoint, keep
        // the configured intent only when that endpoint has one unambiguous
        // NotRequired peer; otherwise an ambiguous endpoint must not merge
        // two independent connection intents.
        var sameEndpoint = peersByIntent
            .Where(existingPeer =>
                !ReferenceEquals(existingPeer, candidate)
                && existingPeer.State == MeshPeerState.NotRequired
                && existingPeer.Direction != candidate.Direction
                && string.Equals(
                    existingPeer.Endpoint,
                    candidate.Endpoint,
                    StringComparison.Ordinal
                )
            )
            .ToArray();
        return sameEndpoint.Length == 1 ? sameEndpoint[0] : null;
    }
}

internal readonly record struct ZLinkMeshPeerExpectation(
    string Endpoint,
    string SecurityIdentity,
    ulong LifecycleGeneration
);

/// <summary>
/// The last observed Core-selected route of each RID (Core ROUTER §10.1). The
/// mesh node receive loop is the socket's single route observer; this type
/// only records what the snapshot reported and never selects a route.
/// </summary>
internal sealed class ZLinkMeshSelectedRoutes
{
    private Dictionary<RoutingId, ulong> _routes = [];

    internal IEnumerable<RoutingId> RoutingIds => _routes.Keys;

    /// <summary>
    /// Replaces the observation and returns every RID whose previously
    /// observed route is missing from the snapshot or has a new generation.
    /// </summary>
    internal IReadOnlyList<RoutingId> Apply(IReadOnlyList<RouterRoute> snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        var next = new Dictionary<RoutingId, ulong>(snapshot.Count);
        foreach (var route in snapshot)
            next[route.RoutingId] = route.RouteGeneration;
        var ended = new List<RoutingId>();
        foreach (var (routingId, generation) in _routes)
            if (!next.TryGetValue(routingId, out var current) || current != generation)
                ended.Add(routingId);
        _routes = next;
        return ended;
    }

    internal void Clear() => _routes.Clear();
}
