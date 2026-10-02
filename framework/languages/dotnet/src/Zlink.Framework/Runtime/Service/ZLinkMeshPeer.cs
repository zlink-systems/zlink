namespace Zlink.Framework.Runtime.Service;

/// <summary>Owns one configured endpoint registration and its connection attempt.</summary>
internal sealed class ZLinkMeshConnectionIntent(
    ulong id,
    string endpoint,
    RoutingId? expectedRid,
    string expectedSecurityIdentity
)
{
    internal ulong Id { get; } = id;
    internal string Endpoint { get; } = endpoint;
    internal RoutingId? ExpectedRid { get; } = expectedRid;
    internal string ExpectedSecurityIdentity { get; } = expectedSecurityIdentity;
    internal RoutingId ResolvedRid { get; set; }
    internal ulong LastChangedMs { get; set; } = checked((ulong)Environment.TickCount64);

    internal bool IsBoundTo(ZLinkMeshPeer? peer) =>
        peer is { Admission: { } admission }
        && (ResolvedRid.IsEmpty ? ExpectedRid == peer.RoutingId : ResolvedRid == peer.RoutingId)
        && ZLinkServiceAdmissionGuard.MatchesExpectedTransportRoute(
            Endpoint,
            ExpectedSecurityIdentity,
            ZLinkServiceSecurityIdentity.Plaintext,
            0,
            admission
        );

    internal bool IsAdmitted(ZLinkMeshPeer? peer) => peer is { Admitted: true } && IsBoundTo(peer);

    internal MeshNodePeer Snapshot(ZLinkMeshPeer? peer)
    {
        var bound = IsBoundTo(peer);
        return new MeshNodePeer(
            Id,
            MeshPeerSource.Manual,
            bound ? peer!.State : MeshPeerState.Connecting,
            ResolvedRid.IsEmpty ? ExpectedRid ?? default : ResolvedRid,
            bound ? peer!.LifecycleGeneration : 0,
            bound ? peer!.DescriptorRevision : 0,
            Endpoint,
            bound ? checked((uint)peer!.Channels.Count) : 0,
            0,
            bound ? peer!.LastChangedMs : LastChangedMs
        )
        {
            ObjectRole =
                bound && peer!.Admission is { } admission
                    ? (ZLinkMeshNodeObjectRole)admission.ObjectRole
                    : ZLinkMeshNodeObjectRole.None,
        };
    }
}

/// <summary>
/// Stores the mutable state for one configured or admitted mesh connection.
/// </summary>
internal sealed class ZLinkMeshPeer(ulong snapshotId)
{
    internal ulong SnapshotId { get; } = snapshotId;
    internal RoutingId RoutingId { get; set; }
    internal RoutingId PhysicalRoutingId { get; set; }

    /// <summary>
    /// The Core route generation (Core ROUTER §10.1) of this peer's handshake:
    /// the route that delivered its admission record, or the observed route
    /// its Hello greeted. 0 while it has no handshake route.
    /// </summary>
    internal ulong RouteGeneration { get; set; }
    internal ulong LifecycleGeneration { get; set; }
    internal ulong DescriptorRevision { get; set; }
    internal IReadOnlyDictionary<string, uint> Channels { get; set; } =
        new Dictionary<string, uint>(StringComparer.Ordinal);
    internal ZLinkServiceWireCodec.AdmissionRecord? Admission { get; set; }
    internal MeshPeerState State { get; set; } = MeshPeerState.Configured;
    internal bool Admitted { get; set; }
    internal ZLinkServiceLiveness? Liveness { get; set; }
    internal ulong LastChangedMs { get; set; } = checked((ulong)Environment.TickCount64);

    internal MeshNodePeer Snapshot() =>
        new(
            SnapshotId,
            MeshPeerSource.Manual,
            State,
            RoutingId,
            LifecycleGeneration,
            DescriptorRevision,
            Admission?.AdvertisedEndpoint ?? string.Empty,
            checked((uint)Channels.Count),
            0,
            LastChangedMs
        )
        {
            ObjectRole = Admission is { } admission
                ? (ZLinkMeshNodeObjectRole)admission.ObjectRole
                : ZLinkMeshNodeObjectRole.None,
        };
}
