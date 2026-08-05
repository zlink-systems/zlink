using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests;

public sealed class TopologyExactSurfaceTests
{
    [Fact]
    public void Public_builders_match_the_exact_topology_surface()
    {
        var frameworkMethods = MethodNames<IZLinkFrameworkOptions>();
        Assert.Contains(nameof(IZLinkFrameworkOptions.ConfigureNetwork), frameworkMethods);
        Assert.Contains(nameof(IZLinkFrameworkOptions.ConfigureInboundDispatch), frameworkMethods);
        Assert.DoesNotContain("ActorTransferTimeout", PropertyNames<IZLinkFrameworkOptions>());
        Assert.DoesNotContain("ActorTransferForwardWindow", PropertyNames<IZLinkFrameworkOptions>());

        var meshMethods = MethodNames<IZLinkMeshNodeBuilder>();
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.Channel), meshMethods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.Listen), meshMethods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetBindHost), meshMethods);
        Assert.Contains(nameof(IZLinkMeshNodeBuilder.SetAdvertiseHost), meshMethods);
        Assert.DoesNotContain("ChannelName", meshMethods);
        Assert.DoesNotContain("UseAllocatedRoutingId", meshMethods);
        Assert.DoesNotContain("SetRoutingIdAllocationGroup", meshMethods);
        Assert.DoesNotContain("SetObjectCapacity", meshMethods);

        var streamMethods = typeof(IZLinkStreamNodeBuilder).GetMethods();
        Assert.Contains(streamMethods, static method =>
            method.Name == nameof(IZLinkStreamNodeBuilder.Bind)
            && method.GetParameters() is [{ ParameterType: var type }]
            && type == typeof(int));
        var actorDispatch = Assert.Single(
            streamMethods,
            static method => method.Name == nameof(IZLinkStreamNodeBuilder.EnableActorDispatch));
        Assert.Empty(actorDispatch.GetParameters());
        Assert.Contains(streamMethods, static method =>
            method.Name == nameof(IZLinkStreamNodeBuilder.ConfigureSocket)
            && method.ReturnType == typeof(IZLinkSocketConfig));
    }

    [Fact]
    public void Root_network_defaults_and_listener_overrides_are_owned_by_registration()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            var network = options.ConfigureNetwork();
            network.BindHost = "0.0.0.0";
            network.AdvertiseHost = "node.example.net";

            options.AddRouteMesh("mesh")
                .Listen()
                .SetBindHost("127.0.0.2")
                .SetAdvertiseHost("mesh.example.net");
            var stream = options.AddStreamNode("stream")
                .Bind()
                .SetBindHost("127.0.0.3")
                .SetAdvertiseHost("stream.example.net");
            stream.ConfigureSocket().MaxMessageSize = 4096;
            stream.AddSession<TestSession>();
        });

        using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        Assert.Equal("0.0.0.0", registration.NetworkOptions.BindHost);
        Assert.Equal("node.example.net", registration.NetworkOptions.AdvertiseHost);

        var router = registration.SpotNodes["mesh"].Router!;
        Assert.Equal(0, router.ListenPort);
        Assert.Equal("127.0.0.2", router.BindHost);
        Assert.Equal("mesh.example.net", router.AdvertiseHost);

        var stream = registration.StreamNodes["stream"];
        Assert.Equal(0, stream.ListenPort);
        Assert.Equal("127.0.0.3", stream.BindHost);
        Assert.Equal("stream.example.net", stream.AdvertiseHost);
        Assert.Equal(4096, stream.SocketConfig.MaxMessageSize);
    }

    private static HashSet<string> MethodNames<T>() =>
        typeof(T).GetMethods().Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);

    private static HashSet<string> PropertyNames<T>() =>
        typeof(T).GetProperties().Select(static property => property.Name)
            .ToHashSet(StringComparer.Ordinal);

    private sealed class TestSession : IZLinkSession
    {
        public IZLinkSessionContext Context => null!;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }
}
