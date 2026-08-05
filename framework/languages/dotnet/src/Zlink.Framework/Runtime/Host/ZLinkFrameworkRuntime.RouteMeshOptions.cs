using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Host;

// RouteMesh·MeshNode runtime-option resolution (spec 05-route-mesh §5, S8-02A).
// A "mesh" is the process-local MeshNode registered under its MeshName; in the
// current surface that is the spot node keyed by SpotNodeName. Weight resolution
// reaches the live IMeshNode through ZLinkSpotNodeRuntime.Node.
internal sealed partial class ZLinkFrameworkRuntime
{
    internal IZLinkMeshPlacementRuntimeOptions ResolveMeshPlacementRuntimeOptions(
        string meshName)
    {
        return ExecuteOperation<IZLinkMeshPlacementRuntimeOptions>(() =>
        {
            var (nodeRuntime, registration) = ResolveMeshNode(meshName);
            return new ZLinkMeshPlacementRuntimeOptions(
                this,
                registration,
                nodeRuntime.Node);
        });
    }

    internal IZLinkMeshChannelRuntimeOptions ResolveMeshChannelRuntimeOptions(
        string channelName)
    {
        return ExecuteOperation<IZLinkMeshChannelRuntimeOptions>(() =>
        {
            var state = GetOrStartState();
            foreach (var registration in Registration.SpotNodes.Values)
            {
                var membership = registration.ChannelMemberships.FirstOrDefault(
                    candidate => candidate.IsServer
                        && string.Equals(
                        candidate.ChannelName,
                        channelName,
                        StringComparison.Ordinal));
                if (membership is null)
                    continue;
                if (!state.SpotNodes.TryGetValue(
                        registration.SpotNodeName,
                        out var nodeRuntime))
                    break;
                return new ZLinkMeshChannelRuntimeOptions(
                    this,
                    nodeRuntime.Node,
                    membership,
                    registration.SpotNodeName,
                    null,
                    null);
            }
            if (Registration.Channels.TryGetValue(channelName, out var channel)
                && channel.Server is { } server
                && state.ClientServerServerBundles.TryGetValue(
                    channelName,
                    out var bundle)
                && bundle.ClientServerServer is { } identity)
                return new ZLinkMeshChannelRuntimeOptions(
                    this,
                    null,
                    null,
                    null,
                    server,
                    identity);
            throw new ZLinkConfigurationException(
                $"No local RouteMesh or ClientServer Server membership '{channelName}' is registered.");
        });
    }

    internal void SetMeshPlacementWeight(
        IZLinkBackendSpotNode node,
        ZLinkSpotNodeRegistration registration,
        int weight)
    {
        _ = node;
        ExecuteOperation(() =>
        {
            registration.PlacementWeight = weight;
            _autoConnect?.SetLocalPlacementWeight(
                registration.SpotNodeName,
                weight);
            return true;
        });
    }

    internal void SetMeshChannelWeight(
        IZLinkBackendSpotNode node,
        ZLinkMeshChannelMembership membership,
        string meshName,
        int weight)
    {
        ExecuteOperation(() =>
        {
            node.SetChannelWeight(membership.ChannelName, (uint)weight);
            membership.Weight = weight;
            _autoConnect?.SetLocalChannelWeight(
                meshName,
                membership.ChannelName,
                weight);
            return true;
        });
    }

    internal void SetClientServerWeight(
        ZLinkChannelServerCapabilityRegistration registration,
        ZLinkClientServerServerIdentity identity,
        int weight)
    {
        ExecuteOperation(() =>
        {
            identity.SetWeight(weight);
            registration.SocketConfig.Weight = weight;
            _autoConnect?.SetClientServerWeight(identity.ChannelName, weight);
            return true;
        });
    }

    private (ZLinkSpotNodeRuntime NodeRuntime, ZLinkSpotNodeRegistration Registration) ResolveMeshNode(
        string meshName)
    {
        if (!Registration.SpotNodes.TryGetValue(meshName, out var registration))
            throw new ZLinkConfigurationException(
                $"RouteMesh '{meshName}' is not registered on this node.");

        var state = GetOrStartState();
        if (!state.SpotNodes.TryGetValue(meshName, out var nodeRuntime))
            throw new ZLinkConfigurationException(
                $"RouteMesh '{meshName}' is not running on this node.");

        return (nodeRuntime, registration);
    }
}
