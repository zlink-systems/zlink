using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkSpotRouteRouterDispatcher(
    Func<ZLinkFrameworkComponentState> getState)
{
    public ValueTask<ZLinkOneWaySubmitResult> SendAsync(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        return ResolveMeshNode(routerChannelId).EntryOutbound.SendToSpotAsync(
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            parts,
            cancellationToken,
            metadata);
    }

    /// <summary>Performs the first non-blocking Spot-send admission attempt
    /// through the MeshNode named by the resolved handle.</summary>
    public bool TrySendOnce(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata = default)
    {
        return ResolveMeshNode(routerChannelId).EntryOutbound.TrySendToSpotOnce(
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            parts,
            metadata);
    }

    public async ValueTask<IReadOnlyList<Message>> RequestAsync(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        return await ResolveMeshNode(routerChannelId).EntryOutbound.RequestToSpotAsync(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                parts,
                timeout,
                cancellationToken,
                metadata)
            .ConfigureAwait(false);
    }

    public RoutingId ResolveAcceptedSpotRouteNodeRid(string targetSpotNodeChannelName)
    {
        return ResolveMeshNode(targetSpotNodeChannelName).Node.RoutingId;
    }

    private ZLinkSpotNodeRuntime ResolveMeshNode(string meshName)
    {
        var state = getState();
        if (state.SpotNodes.TryGetValue(meshName, out var node)
            && node.Registration.Router is not null)
            return node;
        throw new ZLinkConfigurationException(
            $"RouteMesh '{meshName}' is not registered with a router-capable MeshNode.");
    }
}
