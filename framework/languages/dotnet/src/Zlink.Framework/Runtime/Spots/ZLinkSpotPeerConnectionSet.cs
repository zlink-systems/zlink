using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotPeerConnectionSet
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<string, RoutingId?> _routerAuto =
        new(StringComparer.Ordinal);
    private readonly HashSet<string> _routerManual = new(StringComparer.Ordinal);
    private readonly Dictionary<string, RoutingId> _retainedManualPeerRids =
        new(StringComparer.Ordinal);

    public bool TryAddPeerManual(string endpoint)
    {
        return AwaitStateLane(_lane.RunAsync<bool>(() =>
        {
            return Acquire(_routerManual, endpoint);
        }));
    }

    public ZLinkSpotAutoPeerClaim AcquirePeerAuto(
        RoutingId? peerRid,
        string endpoint)
    {
        return AwaitStateLane(_lane.RunAsync<ZLinkSpotAutoPeerClaim>(() =>
        {
            if (_routerAuto.TryGetValue(endpoint, out var current))
            {
                if (SamePeer(current, peerRid))
                    return new(ZLinkSpotAutoPeerClaimKind.AlreadyOwned, null);

                _routerAuto[endpoint] = peerRid;
                return new(
                    _routerManual.Contains(endpoint)
                        ? ZLinkSpotAutoPeerClaimKind.SuppressedByManual
                        : ZLinkSpotAutoPeerClaimKind.Replaced,
                    current);
            }

            _routerAuto[endpoint] = peerRid;
            return new(
                _routerManual.Contains(endpoint)
                    ? ZLinkSpotAutoPeerClaimKind.SuppressedByManual
                    : ZLinkSpotAutoPeerClaimKind.Added,
                null);
        }));
    }

    public bool RemovePeerManual(string endpoint)
    {
        return AwaitStateLane(_lane.RunAsync(() => Release(_routerManual, endpoint)));
    }

    public bool RemovePeerAuto(RoutingId? peerRid, string endpoint)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_routerAuto.TryGetValue(endpoint, out var current)
                || peerRid is { Size: > 0 } expected
                    && !SamePeer(current, expected))
                return false;

            _routerAuto.Remove(endpoint);
            return !IsOwned(endpoint);
        }));
    }

    public void RollbackPeerManual(string endpoint) =>
        AwaitStateLane(_lane.RunAsync(() => { _routerManual.Remove(endpoint); }));
    public void RollbackPeerAuto(string endpoint) =>
        AwaitStateLane(_lane.RunAsync(() => { _routerAuto.Remove(endpoint); }));

    public void RestorePeerAuto(string endpoint, RoutingId? peerRid)
    {
        AwaitStateLane(_lane.RunAsync(() => { _routerAuto[endpoint] = peerRid; }));
    }

    public void RetainManualPeerRid(string endpoint, RoutingId peerRid)
    {
        AwaitStateLane(_lane.RunAsync(() => { _retainedManualPeerRids[endpoint] = peerRid; }));
    }

    public bool HasRetainedManualPeer(RoutingId peerRid)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
            _retainedManualPeerRids.Values.Contains(peerRid)));
    }

    private bool Acquire(HashSet<string> source, string endpoint)
    {
        var alreadyOwned = IsOwned(endpoint);
        return source.Add(endpoint) && !alreadyOwned;
    }

    private bool Release(HashSet<string> source, string endpoint)
    {
        if (!source.Remove(endpoint)) return false;
        return !IsOwned(endpoint);
    }

    private bool IsOwned(string endpoint)
        => _routerManual.Contains(endpoint)
           || _routerAuto.ContainsKey(endpoint);

    private static bool SamePeer(RoutingId? left, RoutingId? right) =>
        left is { Size: > 0 } leftRid && right is { Size: > 0 } rightRid
            ? leftRid == rightRid
            : left is not { Size: > 0 } && right is not { Size: > 0 };

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

}

internal enum ZLinkSpotAutoPeerClaimKind
{
    Added,
    AlreadyOwned,
    Replaced,
    SuppressedByManual
}

internal readonly record struct ZLinkSpotAutoPeerClaim(
    ZLinkSpotAutoPeerClaimKind Kind,
    RoutingId? PreviousPeerRid);
