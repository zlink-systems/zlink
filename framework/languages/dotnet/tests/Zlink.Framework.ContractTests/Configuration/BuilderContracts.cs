using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Configuration;

public sealed class BuilderContracts
{
    [Fact]
    [ContractExample(
        typeof(IZLinkFrameworkOptions),
        typeof(IZLinkInboundDispatchOptions),
        typeof(IZLinkNetworkOptions),
        typeof(IZLinkMeshNodeBuilder),
        typeof(IZLinkMeshChannelRoleBuilder),
        typeof(IZLinkMeshChannelClientBuilder),
        typeof(IZLinkMeshChannelServerBuilder),
        typeof(IZLinkFanoutChannelBuilder),
        typeof(IZLinkFanoutRuntime),
        typeof(IZLinkStreamNodeBuilder),
        typeof(IZLinkStreamCompressionBuilder),
        typeof(IZLinkMeshPeerConnections),
        typeof(IZLinkMeshNodeSocketConfig),
        typeof(IZLinkRouteMeshRuntimeOptions),
        typeof(IZLinkMeshPlacementRuntimeOptions),
        typeof(IZLinkMeshChannelRuntimeOptions),
        typeof(IZLinkRouteMeshRuntime),
        typeof(IZLinkMetadataPolicyBuilder),
        typeof(IZLinkEndpointConnections),
        typeof(IZLinkSocketConfig),
        typeof(IZLinkRouteConfig),
        typeof(IZLinkOutboundRouteConfig),
        typeof(IZLinkSpotPublisherConfig),
        typeof(IZLinkSpotSubscriberConfig))]
    public void Framework_options_expose_the_10_0_registration_surface()
    {
        var methods = typeof(IZLinkFrameworkOptions)
            .GetMethods()
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);

        Assert.Contains(nameof(IZLinkFrameworkOptions.AddRouteMesh), methods);
        Assert.Contains(nameof(IZLinkFrameworkOptions.ConfigureInboundDispatch), methods);
        Assert.Contains(nameof(IZLinkFrameworkOptions.AddFanoutChannel), methods);
        Assert.Contains(nameof(IZLinkFrameworkOptions.AddStreamNode), methods);
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkMeshNodeBuilder),
        typeof(IZLinkMeshNodeSocketConfig),
        typeof(IZLinkMeshPeerConnections))]
    public void Mesh_node_builder_owns_transport_identity_and_direct_route_handlers()
    {
        var methods = typeof(IZLinkMeshNodeBuilder)
            .GetMethods()
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);

        Assert.Contains(nameof(IZLinkMeshNodeBuilder.Listen), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetRoutingId), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetRoutingIdPrefix), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetActorLimit), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetSpotLimit), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetActivationConcurrency), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.Channel), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetBindHost), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetAdvertiseHost), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.AddRouteSendHandler), methods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.AddRouteRequestHandler), methods);
        Assert.DoesNotContain("ChannelName", methods);
        Assert.DoesNotContain("UseAllocatedRoutingId", methods);
        Assert.DoesNotContain("SetObjectCapacity", methods);
        Assert.NotNull(typeof(IZLinkMeshNodeBuilder).GetProperty(nameof(IZLinkMeshNodeBuilder.PeerConnections)));
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkMeshChannelRoleBuilder),
        typeof(IZLinkMeshChannelClientBuilder),
        typeof(IZLinkMeshChannelServerBuilder))]
    public void Mesh_channel_role_separates_client_from_server_configuration()
    {
        Assert.Equal(
            typeof(IZLinkMeshChannelClientBuilder),
            typeof(IZLinkMeshChannelRoleBuilder)
                .GetMethod(nameof(IZLinkMeshChannelRoleBuilder.Client))!
                .ReturnType);
        Assert.Equal(
            typeof(IZLinkMeshChannelServerBuilder),
            typeof(IZLinkMeshChannelRoleBuilder)
                .GetMethod(nameof(IZLinkMeshChannelRoleBuilder.Server))!
                .ReturnType);

        Assert.Empty(typeof(IZLinkMeshChannelClientBuilder).GetMethods());
        var serverMethods = typeof(IZLinkMeshChannelServerBuilder)
            .GetMethods()
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.Contains(nameof(IZLinkMeshChannelServerBuilder.SetWeight), serverMethods);
        Assert.Contains(nameof(IZLinkMeshChannelServerBuilder.AddSendHandler), serverMethods);
        Assert.Contains(nameof(IZLinkMeshChannelServerBuilder.AddRequestHandler), serverMethods);
    }

}
