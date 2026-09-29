using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotPeerConnector(
    IZLinkBackendSpotNode node,
    ZLinkSpotPeerConnectionSet connections
)
{
    private readonly ZLinkStateLane _lane = new();

    // Manual entry points accept an endpoint straight from the caller (an
    // external boundary per the endpoint notation policy); normalize once
    // here so a differently-notated Disconnect still matches an earlier
    // Connect and set membership stays consistent with the reconciler's
    // already-normalized automatic targets.
    public ValueTask<bool> ConnectPeerAsync(string endpoint, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        endpoint = ZLinkEndpointNotation.Normalize(endpoint);
        return ValueTask.FromResult(
            AwaitStateLane(_lane.RunAsync(() => ConnectPeerManual(endpoint)))
        );
    }

    public ValueTask<bool> ConnectPeerAsync(
        RoutingId peerRid,
        string endpoint,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        endpoint = ZLinkEndpointNotation.Normalize(endpoint);
        return ValueTask.FromResult(
            AwaitStateLane(_lane.RunAsync(() => ConnectPeerManual(peerRid, endpoint)))
        );
    }

    public void Disconnect(string endpoint)
    {
        DisconnectPeerManual(endpoint);
    }

    public void DisconnectPeerManual(string endpoint)
    {
        endpoint = ZLinkEndpointNotation.Normalize(endpoint);
        AwaitStateLane(_lane.RunAsync(() => DisconnectPeerManualCore(endpoint)));
    }

    public bool ConnectPeerAuto(
        RoutingId? peerRid,
        string endpoint,
        string expectedSecurityIdentity
    ) => AwaitStateLane(ConnectPeerAutoAsync(peerRid, endpoint, expectedSecurityIdentity));

    public ValueTask<bool> ConnectPeerAutoAsync(
        RoutingId? peerRid,
        string endpoint,
        string expectedSecurityIdentity
    ) =>
        _lane.RunAwaitingAsync(async () =>
        {
            var claim = await connections
                .AcquirePeerAutoAsync(peerRid, endpoint)
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"spot_peer_claim peer={peerRid?.ToString() ?? "<unknown>"} endpoint={endpoint} kind={claim.Kind} "
                    + $"previous={claim.PreviousPeerRid?.ToString() ?? "<unknown>"}"
            );
            if (
                claim.Kind
                is ZLinkSpotAutoPeerClaimKind.AlreadyOwned
                    or ZLinkSpotAutoPeerClaimKind.SuppressedByManual
            )
                return true;

            try
            {
                if (claim.Kind == ZLinkSpotAutoPeerClaimKind.Replaced)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"spot_peer_replace peer={peerRid?.ToString() ?? "<unknown>"} endpoint={endpoint}"
                    );
                    await node.DisconnectPeerAsync(endpoint).ConfigureAwait(false);
                }

                if (peerRid is { Size: > 0 } rid)
                    await node.ConnectPeerAsync(rid, endpoint, expectedSecurityIdentity)
                        .ConfigureAwait(false);
                else
                    await node.ConnectPeerAsync(endpoint).ConfigureAwait(false);
                return true;
            }
            catch
            {
                // A failed replacement leaves no physical connection that
                // the old target can safely reuse. Remove the claim so the
                // reconciler retries the currently desired target.
                await connections.RollbackPeerAutoAsync(endpoint).ConfigureAwait(false);
                return false;
            }
        });

    public bool DisconnectPeerAuto(string endpoint) => DisconnectPeerAuto(peerRid: null, endpoint);

    public bool DisconnectPeerAuto(RoutingId? peerRid, string endpoint) =>
        AwaitStateLane(DisconnectPeerAutoAsync(peerRid, endpoint));

    public ValueTask<bool> DisconnectPeerAutoAsync(RoutingId? peerRid, string endpoint) =>
        _lane.RunAwaitingAsync(async () =>
        {
            var result = await DisconnectAutoAsync(
                    peerRid,
                    endpoint,
                    () => connections.RemovePeerAutoAsync(peerRid, endpoint),
                    () => connections.RestorePeerAutoAsync(endpoint, peerRid)
                )
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"spot_peer_release peer={peerRid?.ToString() ?? "<unknown>"} endpoint={endpoint} result={result}"
            );
            return result;
        });

    public bool DisconnectPeerBeforeAdmission(
        RoutingId peerRid,
        string endpoint,
        ulong lifecycleGeneration
    ) => AwaitStateLane(DisconnectPeerBeforeAdmissionAsync(peerRid, endpoint, lifecycleGeneration));

    public ValueTask<bool> DisconnectPeerBeforeAdmissionAsync(
        RoutingId peerRid,
        string endpoint,
        ulong lifecycleGeneration
    ) =>
        _lane.RunAwaitingAsync(async () =>
        {
            try
            {
                return await node.DisconnectPeerBeforeAdmissionAsync(
                    peerRid,
                    endpoint,
                    lifecycleGeneration
                );
            }
            catch
            {
                return false;
            }
        });

    private bool ConnectPeerManual(string endpoint)
    {
        if (!connections.TryAddPeerManual(endpoint))
            return false;
        try
        {
            ConnectPeer(endpoint);
        }
        catch
        {
            connections.RollbackPeerManual(endpoint);
            throw;
        }
        return true;
    }

    private bool ConnectPeerManual(RoutingId peerRid, string endpoint)
    {
        if (!connections.TryAddPeerManual(endpoint))
            return false;
        try
        {
            ConnectPeer(peerRid, endpoint, ZLinkServiceSecurityIdentity.Plaintext);
        }
        catch
        {
            connections.RollbackPeerManual(endpoint);
            throw;
        }
        return true;
    }

    private void DisconnectPeerManualCore(string endpoint)
    {
        if (!connections.RemovePeerManual(endpoint))
            return;
        try
        {
            node.DisconnectPeer(endpoint);
        }
        catch
        {
            _ = connections.TryAddPeerManual(endpoint);
            throw;
        }
    }

    private async ValueTask<bool> DisconnectAutoAsync(
        RoutingId? peerRid,
        string endpoint,
        Func<ValueTask<bool>> release,
        Func<ValueTask> restore
    )
    {
        var released = await release().ConfigureAwait(false);
        // A different auto target may already own the endpoint after a RID
        // replacement. The old physical peer still requires exact cleanup;
        // only an endpoint-only release can return without a transport step.
        if (!released && peerRid is not { Size: > 0 })
            return true;
        try
        {
            if (peerRid is { Size: > 0 } rid)
            {
                await DisconnectPeerLifetimeAsync(rid, endpoint).ConfigureAwait(false);
            }
            else
            {
                await node.DisconnectPeerAsync(endpoint).ConfigureAwait(false);
            }
            return true;
        }
        catch
        {
            if (released)
                await restore().ConfigureAwait(false);
            return false;
        }
    }

    private async ValueTask DisconnectPeerLifetimeAsync(RoutingId peerRid, string endpoint)
    {
        foreach (var peer in await node.MeshPeersAsync().ConfigureAwait(false))
        {
            if (
                peer.RoutingId != peerRid
                || !string.Equals(peer.Endpoint, endpoint, StringComparison.Ordinal)
            )
                continue;

            if (peer.State is MeshPeerState.Admitted or MeshPeerState.Draining)
            {
                await node.DisconnectPeerLifetimeAsync(peerRid, peer.LifecycleGeneration)
                    .ConfigureAwait(false);
            }
            else
            {
                await node.DisconnectPeerBeforeAdmissionAsync(
                        peerRid,
                        endpoint,
                        peer.LifecycleGeneration
                    )
                    .ConfigureAwait(false);
            }
            return;
        }
    }

    private void ConnectPeer(string endpoint)
    {
        node.ConnectPeer(endpoint);
    }

    private void ConnectPeer(RoutingId peerRid, string endpoint, string expectedSecurityIdentity)
    {
        node.ConnectPeer(peerRid, endpoint, expectedSecurityIdentity);
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation)
    {
        global::Zlink.Framework.Runtime.Execution.ZLinkInfrastructureWaitGuard.ThrowIfBlocking(
            operation.IsCompleted,
            "state lane"
        );
        return operation.GetAwaiter().GetResult();
    }

    private static void AwaitStateLane(ValueTask operation)
    {
        global::Zlink.Framework.Runtime.Execution.ZLinkInfrastructureWaitGuard.ThrowIfBlocking(
            operation.IsCompleted,
            "state lane"
        );
        operation.GetAwaiter().GetResult();
    }
}
