namespace Zlink.Framework.Runtime.Service;

/// <summary>
/// Stores the mutable state for one configured or admitted mesh connection.
/// </summary>
internal sealed class ZLinkMeshPeer(
    ulong intent,
    string endpoint,
    RoutingId? expectedRid,
    string expectedSecurityIdentity,
    ZLinkServiceConnectionDirection direction,
    ulong connectionGeneration = 0)
{
    internal ulong Intent { get; } = intent;
    internal string Endpoint { get; } = endpoint;
    internal RoutingId? ExpectedRid { get; } = expectedRid;
    internal string ExpectedSecurityIdentity { get; } =
        expectedSecurityIdentity;
    internal ZLinkServiceConnectionDirection Direction { get; } = direction;
    internal string Discriminator { get; } =
        $"{(direction == ZLinkServiceConnectionDirection.Outbound ? "out" : "in")}:" +
        $"{endpoint}:{intent:x16}";
    internal ulong ConnectionGeneration { get; set; } = connectionGeneration;
    internal RoutingId RoutingId { get; set; }
    internal RoutingId PhysicalRoutingId { get; set; }
    internal ulong LifecycleGeneration { get; set; }
    internal ulong DescriptorRevision { get; set; }
    internal IReadOnlyDictionary<string, uint> Channels { get; set; } =
        new Dictionary<string, uint>(StringComparer.Ordinal);
    internal ZLinkServiceWireCodec.AdmissionRecord? Admission { get; set; }
    // Native request reply tokens are scoped to the exact paired transport
    // that delivered the request. The logical RID can survive a ROUTER
    // handover, so it is not a physical-connection fence.
    internal ZLinkTransportPairIdentity TransportPair { get; set; }
    internal ZLinkNativeReplyPeerEpoch NativeReplyEpoch { get; set; } = new();
    internal MeshPeerState State { get; set; } = MeshPeerState.Configured;
    internal bool Admitted { get; set; }
    internal ZLinkServiceLiveness? Liveness { get; set; }
    internal long NextAdmissionTimestamp { get; set; }
    internal ulong LastChangedMs { get; set; } =
        checked((ulong)Environment.TickCount64);

    internal MeshNodePeer Snapshot() =>
        new(
            Intent,
            MeshPeerSource.Manual,
            State,
            RoutingId.IsEmpty ? ExpectedRid ?? default : RoutingId,
            LifecycleGeneration,
            DescriptorRevision,
            Endpoint,
            checked((uint)Channels.Count),
            0,
            LastChangedMs)
        {
            ObjectRole = Admission is { } admission
                ? (ZLinkMeshNodeObjectRole)admission.ObjectRole
                : ZLinkMeshNodeObjectRole.None
        };
}

internal readonly record struct ZLinkTransportPairIdentity(
    ulong Id,
    ulong Generation)
{
    internal bool IsValid => Id != 0;
}

internal sealed class ZLinkNativeReplyPeerEpoch
{
    private int _invalidated;

    internal ZLinkNativeReplyPeerEpoch(
        ZLinkTransportPairIdentity transportPair = default)
    {
        TransportPair = transportPair;
    }

    internal ZLinkTransportPairIdentity TransportPair { get; private set; }
    internal bool IsValid => Volatile.Read(ref _invalidated) == 0;

    internal bool TryAttach(ZLinkTransportPairIdentity transportPair)
    {
        if (!transportPair.IsValid)
            return false;
        if (TransportPair.IsValid)
            return TransportPair == transportPair;
        TransportPair = transportPair;
        return true;
    }

    internal void Invalidate() => Interlocked.Exchange(ref _invalidated, 1);
}
