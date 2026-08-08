using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Service;

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
            try { ConnectPeer(peerRid, endpoint, ZLinkServiceSecurityIdentity.Plaintext); }
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
                peerRid,
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
        RoutingId? peerRid,
        string endpoint,
        Func<bool> release,
        Action restore)
    {
        var released = release();
        // A different auto target may already own the endpoint after a RID
        // replacement. The old physical peer still requires exact cleanup;
        // only an endpoint-only release can return without a transport step.
        if (!released && peerRid is not { Size: > 0 }) return true;
        try
        {
            if (peerRid is { Size: > 0 } rid)
            {
                DisconnectPeerLifetime(rid, endpoint);
            }
            else
            {
                node.DisconnectPeer(endpoint);
            }
            return true;
        }
        catch
        {
            if (released) restore();
            return false;
        }
    }

    private void DisconnectPeerLifetime(RoutingId peerRid, string endpoint)
    {
        foreach (var peer in node.MeshPeers())
        {
            if (peer.RoutingId != peerRid
                || !string.Equals(peer.Endpoint, endpoint, StringComparison.Ordinal))
                continue;

            if (peer.State is MeshPeerState.Admitted or MeshPeerState.Draining)
            {
                node.DisconnectPeerLifetime(peerRid, peer.LifecycleGeneration);
            }
            else
            {
                node.DisconnectPeerBeforeAdmission(
                    peerRid,
                    endpoint,
                    peer.LifecycleGeneration);
            }
            return;
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
