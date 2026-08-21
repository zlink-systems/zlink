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
    // A native request reply token is scoped to the transport that delivered
    // its request. A replacement inbound Hello retires the prior epoch before
    // its late disconnect monitor event can be processed.
    internal ZLinkNativeReplyPeerEpoch NativeReplyEpoch { get; set; } = new();
    // The public monitor event does not expose Core's physical connection
    // identity. After a replacement Hello, one subsequently observed close
    // can therefore belong to the retired transport even if it is delivered
    // after the new Hello. Do not let that ambiguous close kill the new epoch.
    internal bool NativeReplyEpochHasRetiredDisconnect { get; set; }
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

internal sealed class ZLinkNativeReplyPeerEpoch
{
    private int _invalidated;

    internal bool IsValid => Volatile.Read(ref _invalidated) == 0;

    internal void Invalidate() => Interlocked.Exchange(ref _invalidated, 1);
}
