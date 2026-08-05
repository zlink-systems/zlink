using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.Runtime.Channels;

// Public DI singleton implementation of the exact runtime weight surface.
internal sealed class ZLinkRouteMeshRuntimeOptionsService(ZLinkFrameworkRuntime runtime)
    : IZLinkRouteMeshRuntimeOptions
{
    public IZLinkMeshPlacementRuntimeOptions Mesh(string meshName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        return runtime.ResolveMeshPlacementRuntimeOptions(meshName);
    }

    public IZLinkMeshChannelRuntimeOptions Channel(string channelName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(channelName);
        return runtime.ResolveMeshChannelRuntimeOptions(channelName);
    }
}

internal sealed class ZLinkMeshPlacementRuntimeOptions(
    ZLinkFrameworkRuntime runtime,
    ZLinkSpotNodeRegistration registration,
    IZLinkBackendSpotNode node)
    : IZLinkMeshPlacementRuntimeOptions
{
    public int PlacementWeight
    {
        get => registration.PlacementWeight;
        set
        {
            ZLinkSocketConfig.ValidatePeerWeight(value);
            runtime.SetMeshPlacementWeight(node, registration, value);
        }
    }
}

internal sealed class ZLinkMeshChannelRuntimeOptions(
    ZLinkFrameworkRuntime runtime,
    IZLinkBackendSpotNode? node,
    ZLinkMeshChannelMembership? membership,
    string? meshName,
    ZLinkChannelServerCapabilityRegistration? clientServer,
    ZLinkClientServerServerIdentity? clientServerIdentity)
    : IZLinkMeshChannelRuntimeOptions
{
    public int Weight
    {
        get => membership?.Weight ?? clientServer!.SocketConfig.Weight;
        set
        {
            ZLinkSocketConfig.ValidatePeerWeight(value);
            if (membership is not null)
                runtime.SetMeshChannelWeight(node!, membership, meshName!, value);
            else
                runtime.SetClientServerWeight(
                    clientServer!,
                    clientServerIdentity!,
                    value);
        }
    }
}
