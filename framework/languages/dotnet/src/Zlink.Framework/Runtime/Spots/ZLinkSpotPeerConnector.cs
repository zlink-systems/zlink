using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotPeerConnector(
    IZLinkBackendSpotNode node,
    ZLinkSpotPeerConnectionSet connections)
{
    private readonly object _gate = new();

    public ValueTask<bool> ConnectPeerAsync(string endpoint, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!connections.TryAddPeerManual(endpoint)) return ValueTask.FromResult(false);
            try { ConnectPeer(endpoint); }
            catch { connections.RollbackPeerManual(endpoint); throw; }
            return ValueTask.FromResult(true);
        }
    }

    public ValueTask<bool> ConnectPeerAsync(
        RoutingId peerRid,
        string endpoint,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!connections.TryAddPeerManual(endpoint)) return ValueTask.FromResult(false);
            try { ConnectPeer(peerRid, endpoint, "none"); }
            catch { connections.RollbackPeerManual(endpoint); throw; }
            return ValueTask.FromResult(true);
        }
    }

    public void Disconnect(string endpoint)
    {
        DisconnectPeerManual(endpoint);
    }

    public void DisconnectPeerManual(string endpoint)
    {
        lock (_gate)
        {
            if (!connections.RemovePeerManual(endpoint)) return;
            try { node.DisconnectPeer(endpoint); }
            catch { _ = connections.TryAddPeerManual(endpoint); throw; }
        }
    }

    public bool ConnectPeerAuto(
        RoutingId? peerRid,
        string endpoint,
        string expectedSecurityIdentity)
    {
        lock (_gate)
        {
            var claim = connections.AcquirePeerAuto(peerRid, endpoint);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"spot_peer_claim peer={peerRid?.ToString() ?? "<unknown>"} endpoint={endpoint} kind={claim.Kind} "
                + $"previous={claim.PreviousPeerRid?.ToString() ?? "<unknown>"}");
            if (claim.Kind is ZLinkSpotAutoPeerClaimKind.AlreadyOwned
                or ZLinkSpotAutoPeerClaimKind.SuppressedByManual)
                return true;

            try
            {
                if (claim.Kind == ZLinkSpotAutoPeerClaimKind.Replaced)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"spot_peer_replace peer={peerRid?.ToString() ?? "<unknown>"} endpoint={endpoint}");
                    node.DisconnectPeer(endpoint);
                }

                if (peerRid is { Size: > 0 } rid)
                    ConnectPeer(rid, endpoint, expectedSecurityIdentity);
                else ConnectPeer(endpoint);
                return true;
            }
            catch
            {
                // A failed replacement leaves no physical connection that
                // the old target can safely reuse. Remove the claim so the
                // reconciler retries the currently desired target.
                connections.RollbackPeerAuto(endpoint);
                return false;
            }
        }
    }

    public bool DisconnectPeerAuto(string endpoint) =>
        DisconnectPeerAuto(peerRid: null, endpoint);

    public bool DisconnectPeerAuto(RoutingId? peerRid, string endpoint)
    {
        lock (_gate)
        {
            var result = DisconnectAuto(
                endpoint,
                () => connections.RemovePeerAuto(peerRid, endpoint),
                () => connections.RestorePeerAuto(endpoint, peerRid));
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"spot_peer_release peer={peerRid?.ToString() ?? "<unknown>"} endpoint={endpoint} result={result}");
            return result;
        }
    }

    public bool DisconnectPeerBeforeAdmission(
        RoutingId peerRid,
        string endpoint,
        ulong lifecycleGeneration)
    {
        lock (_gate)
        {
            try
            {
                return node.DisconnectPeerBeforeAdmission(
                    peerRid,
                    endpoint,
                    lifecycleGeneration);
            }
            catch
            {
                return false;
            }
        }
    }

    private bool DisconnectAuto(
        string endpoint,
        Func<bool> release,
        Action restore)
    {
        if (!release()) return true;
        try
        {
            node.DisconnectPeer(endpoint);
            return true;
        }
        catch
        {
            restore();
            return false;
        }
    }

    private void ConnectPeer(string endpoint)
    {
        node.ConnectPeer(endpoint);
    }

    private void ConnectPeer(
        RoutingId peerRid,
        string endpoint,
        string expectedSecurityIdentity)
    {
        node.ConnectPeer(peerRid, endpoint, expectedSecurityIdentity);
    }
}
