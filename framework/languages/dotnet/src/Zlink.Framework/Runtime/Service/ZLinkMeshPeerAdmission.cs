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
    internal static ZLinkMeshPeer? FindReadyOutboundCandidate(
        IEnumerable<ZLinkMeshPeer> peersByIntent,
        string remoteAddress)
    {
        ArgumentNullException.ThrowIfNull(peersByIntent);
        ArgumentNullException.ThrowIfNull(remoteAddress);
        return peersByIntent.FirstOrDefault(peer =>
            peer.Direction == ZLinkServiceConnectionDirection.Outbound
            && peer.State != MeshPeerState.Closed
            && string.Equals(
                peer.Endpoint,
                remoteAddress,
                StringComparison.Ordinal));
    }

    internal ZLinkMeshPeer? FindForAdmission(
        IReadOnlyDictionary<RoutingId, ZLinkMeshPeer> peersByRid,
        IEnumerable<ZLinkMeshPeer> peersByIntent,
        RoutingId sourceRid,
        ServiceWireConstants.Command command,
        string advertisedEndpoint,
        ZLinkServiceConnectionDirection? candidateDirection)
    {
        ArgumentNullException.ThrowIfNull(peersByRid);
        ArgumentNullException.ThrowIfNull(peersByIntent);
        ArgumentNullException.ThrowIfNull(advertisedEndpoint);

        if (peersByRid.TryGetValue(sourceRid, out var exact)
            && (exact.Admitted
                || command == ServiceWireConstants.Command.Update
                || command == ServiceWireConstants.Command.Hello
                    && exact.Direction == ZLinkServiceConnectionDirection.Inbound
                || command == ServiceWireConstants.Command.Admit
                    && exact.Direction == ZLinkServiceConnectionDirection.Outbound))
            return exact;

        var peers = peersByIntent.ToArray();
        if (command == ServiceWireConstants.Command.Hello)
        {
            var inbound = peers
                .Where(peer =>
                    peer.Direction == ZLinkServiceConnectionDirection.Inbound
                    && candidateDirection
                        != ZLinkServiceConnectionDirection.Outbound
                    && !peer.Admitted
                    && (peer.State == MeshPeerState.NotRequired
                        || peer.State == MeshPeerState.Connecting)
                    && peer.RoutingId == sourceRid)
                .OrderBy(static peer => peer.Discriminator, StringComparer.Ordinal)
                .FirstOrDefault();
            if (inbound is not null
                || candidateDirection == ZLinkServiceConnectionDirection.Inbound)
                return inbound;
            // RouteMesh peers can both send Hello on one unilateral physical
            // connection. With no accepted inbound candidate, bind that Hello
            // to the sole configured outbound intent just as the C++ candidate
            // registry falls back to the only opposite-direction connection.
        }

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
        var peers = peersByIntent
            .Where(existingPeer =>
            !ReferenceEquals(existingPeer, candidate)
            && existingPeer.State == MeshPeerState.NotRequired
            && (existingPeer.RoutingId == sourceRid
                || existingPeer.ExpectedRid == sourceRid
                || existingPeer.PhysicalRoutingId == sourceRid))
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
                    StringComparison.Ordinal))
            .ToArray();
        return sameEndpoint.Length == 1 ? sameEndpoint[0] : null;
    }
}

internal readonly record struct ZLinkMeshPeerExpectation(
    string Endpoint,
    string SecurityIdentity,
    ulong LifecycleGeneration);

internal sealed class ZLinkMeshConnectionCandidates
{
    private readonly Dictionary<RoutingId, Dictionary<ulong, Candidate>>
        _candidates = [];
    private ulong _nextReadySequence = 1;

    internal void Ready(
        RoutingId routingId,
        ulong connectionId,
        ZLinkServiceConnectionDirection direction,
        string remoteEndpoint)
    {
        ArgumentNullException.ThrowIfNull(remoteEndpoint);
        if (routingId.IsEmpty || connectionId == 0)
            return;
        if (!_candidates.TryGetValue(routingId, out var connections))
        {
            connections = [];
            _candidates.Add(routingId, connections);
        }
        var readySequence = _nextReadySequence++;
        if (_nextReadySequence == 0)
            _nextReadySequence = 1;
        connections[connectionId] = new Candidate(
            direction,
            remoteEndpoint,
            readySequence);
    }

    internal ZLinkMeshConnectionCandidate? ForHandshake(
        RoutingId routingId,
        ZLinkServiceConnectionDirection preferredDirection)
    {
        if (!_candidates.TryGetValue(routingId, out var connections))
            return null;
        var preferred = connections
            .Where(pair => pair.Value.Pending
                && pair.Value.Direction == preferredDirection)
            .OrderByDescending(static pair => pair.Value.ReadySequence)
            .Select(static pair => new ZLinkMeshConnectionCandidate(
                pair.Key,
                pair.Value.Direction,
                pair.Value.RemoteEndpoint))
            .FirstOrDefault();
        return preferred ?? connections
            .Where(static pair => pair.Value.Pending)
            .OrderByDescending(static pair => pair.Value.ReadySequence)
            .Select(static pair => new ZLinkMeshConnectionCandidate(
                pair.Key,
                pair.Value.Direction,
                pair.Value.RemoteEndpoint))
            .FirstOrDefault();
    }

    internal bool Consume(RoutingId routingId, ulong connectionId)
    {
        if (!_candidates.TryGetValue(routingId, out var connections)
            || !connections.TryGetValue(connectionId, out var candidate)
            || !candidate.Pending)
            return false;
        connections[connectionId] = candidate with { Pending = false };
        return true;
    }

    internal ZLinkMeshConnectionCandidateDisconnect? Disconnect(
        RoutingId routingId,
        ulong connectionId,
        string remoteEndpoint)
    {
        ArgumentNullException.ThrowIfNull(remoteEndpoint);
        if (connectionId == 0)
            return null;

        if (!routingId.IsEmpty
            && TryRemove(routingId, connectionId, remoteEndpoint, out var direct))
            return direct;

        foreach (var candidateRid in _candidates.Keys.ToArray())
        {
            if (TryRemove(
                    candidateRid,
                    connectionId,
                    remoteEndpoint,
                    out var found))
                return found;
        }
        return null;
    }

    internal void Clear() => _candidates.Clear();

    private bool TryRemove(
        RoutingId routingId,
        ulong connectionId,
        string remoteEndpoint,
        out ZLinkMeshConnectionCandidateDisconnect? result)
    {
        result = null;
        if (!_candidates.TryGetValue(routingId, out var connections)
            || !connections.TryGetValue(connectionId, out var candidate)
            || remoteEndpoint.Length != 0
                && !string.Equals(
                    candidate.RemoteEndpoint,
                    remoteEndpoint,
                    StringComparison.Ordinal))
            return false;

        connections.Remove(connectionId);
        var hasRemainingCandidates = connections.Count != 0;
        if (!hasRemainingCandidates)
            _candidates.Remove(routingId);
        result = new ZLinkMeshConnectionCandidateDisconnect(
            routingId,
            hasRemainingCandidates);
        return true;
    }

    private sealed record Candidate(
        ZLinkServiceConnectionDirection Direction,
        string RemoteEndpoint,
        ulong ReadySequence,
        bool Pending = true);
}

internal sealed record ZLinkMeshConnectionCandidate(
    ulong ConnectionId,
    ZLinkServiceConnectionDirection Direction,
    string RemoteEndpoint);

internal sealed record ZLinkMeshConnectionCandidateDisconnect(
    RoutingId RoutingId,
    bool HasRemainingCandidates);
