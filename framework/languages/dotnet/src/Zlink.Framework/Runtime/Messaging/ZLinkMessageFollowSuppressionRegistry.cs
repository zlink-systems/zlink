namespace Zlink.Framework.Runtime.Messaging;

internal enum ZLinkMessageFollowObjectKind : byte
{
    Actor = ZLinkServiceWireCodec.MessageFollowActorKind,
    Spot = ZLinkServiceWireCodec.MessageFollowSpotKind
}

// Every field that can distinguish a stale source or target is part of the
// suppression identity. A route replacement therefore cannot inherit the
// notification state of the route it replaced.
internal readonly record struct ZLinkMessageFollowFence(
    ZLinkMessageFollowObjectKind ObjectKind,
    string SourceLogicalId,
    string TargetLogicalId,
    RoutingId SourceNodeRid,
    RoutingId TargetNodeRid,
    ulong SourceObjectGeneration,
    ulong TargetObjectGeneration,
    ulong SourceNodeGeneration,
    ulong TargetNodeGeneration,
    ulong SourceAuthorityOwnerGeneration,
    ulong TargetAuthorityOwnerGeneration,
    ulong SourceOwnerLeaseGeneration,
    ulong TargetOwnerLeaseGeneration);

internal sealed class ZLinkMessageFollowSuppressionRegistry
{
    private readonly object _gate = new();
    private readonly Dictionary<ZLinkMessageFollowFence, MarkerState> _markers = [];
    private readonly int _capacity;

    internal ZLinkMessageFollowSuppressionRegistry(int capacity = 4_096)
    {
        if (capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        _capacity = capacity;
    }

    // Absence represents idle. Only one caller can change the exact fence to
    // in-flight; sent markers remain until their owning route expires.
    internal bool TryBegin(ZLinkMessageFollowFence fence)
    {
        lock (_gate)
        {
            if (_markers.ContainsKey(fence) || _markers.Count >= _capacity)
                return false;
            _markers.Add(fence, MarkerState.InFlight);
            return true;
        }
    }

    internal bool MarkSent(ZLinkMessageFollowFence fence)
    {
        lock (_gate)
        {
            if (!_markers.TryGetValue(fence, out var state)
                || state != MarkerState.InFlight)
                return false;
            _markers[fence] = MarkerState.SentUntilExpiry;
            return true;
        }
    }

    internal void Abort(ZLinkMessageFollowFence fence)
    {
        lock (_gate)
        {
            if (_markers.GetValueOrDefault(fence) == MarkerState.InFlight)
                _markers.Remove(fence);
        }
    }

    internal void Expire(ZLinkMessageFollowFence fence)
    {
        lock (_gate)
            _markers.Remove(fence);
    }

    internal void ExpireAll()
    {
        lock (_gate)
            _markers.Clear();
    }

    private enum MarkerState : byte
    {
        InFlight,
        SentUntilExpiry
    }
}
