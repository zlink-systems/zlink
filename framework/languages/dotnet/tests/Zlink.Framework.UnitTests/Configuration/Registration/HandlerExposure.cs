using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests;

public sealed class HandlerExposureTests : RegistrationValidationSupport
{
    [Fact]
    public void MeshMembership_RecordsExplicitTypedHandlers()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            var mesh = options.AddRouteMesh("profile")
                .Listen("tcp://127.0.0.1:7101")
                .SetRoutingId(RoutingId.From("profile"));
            mesh.Channel("profile").Server()
                .AddSendHandler<TestSendHandler, TestMessage>("send")
                .AddRequestHandler<TestRequestHandler, TestRequest, TestReply>("request");
        });

        var registration = services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>();
        var membership = Assert.Single(
            Assert.Single(registration.SpotNodes.Values).ChannelMemberships);

        Assert.Equal("profile", membership.ChannelName);
        Assert.Equal("send", Assert.Single(membership.SendHandlers).PacketName);
        Assert.Equal("request", Assert.Single(membership.RequestHandlers).PacketName);
    }

    [Fact]
    public void MeshMembership_RecordsScannedHandlerGroup()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddHandlersFromAssemblyOf(typeof(HandlerExposureTests));
            options.AddRouteMesh("profile")
                .Listen("tcp://127.0.0.1:7101")
                .SetRoutingId(RoutingId.From("profile"))
                .Channel("profile").Server()
                .AddHandlerGroup("mesh-handler-exposure-test");
        });

        var registration = services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>();
        var membership = Assert.Single(
            Assert.Single(registration.SpotNodes.Values).ChannelMemberships);

        Assert.Contains("mesh-handler-exposure-test", membership.HandlerGroups);
        Assert.Empty(membership.RequestHandlers);
    }

    [Fact]
    public void MeshNode_RecordsExplicitDirectRouteHandlers()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            var mesh = options.AddRouteMesh("profile")
                .Listen("tcp://127.0.0.1:7101")
                .SetRoutingId(RoutingId.From("profile"));
            mesh.Channel("profile").Server();
            mesh.AddRouteSendHandler<TestRouteSendHandler, TestMessage>("route-send")
                .AddRouteRequestHandler<ExplicitRouteRequestHandler, TestRequest, TestReply>("route-request");
        });

        var node = Assert.Single(services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .SpotNodes.Values);
        Assert.Equal("route-send", Assert.Single(node.RouteSendHandlers).PacketName);
        Assert.Equal("route-request", Assert.Single(node.RouteRequestHandlers).PacketName);
    }

    [Fact]
    public void MeshMembership_RejectsDuplicateExplicitRequestPacket()
    {
        var services = new ServiceCollection();
        var exception = Assert.Throws<ZLinkConfigurationException>(
            () => services.AddZLinkFramework(options =>
            {
                var mesh = options.AddRouteMesh("profile")
                    .Listen("tcp://127.0.0.1:7101")
                    .SetRoutingId(RoutingId.From("profile"));
                mesh.Channel("profile").Server()
                    .AddRequestHandler<TestRequestHandler, TestRequest, TestReply>("request")
                    .AddRequestHandler<AlternateTestRequestHandler, TestRequest, TestReply>("request");
            }));

        Assert.Contains("Duplicate request handler 'profile:profile:request'", exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public void MeshNode_RejectsDuplicateExplicitRouteRequestPacket()
    {
        var services = new ServiceCollection();
        var exception = Assert.Throws<ZLinkConfigurationException>(
            () => services.AddZLinkFramework(options =>
            {
                var mesh = options.AddRouteMesh("profile")
                    .Listen("tcp://127.0.0.1:7101")
                    .SetRoutingId(RoutingId.From("profile"));
                mesh.Channel("profile").Server();
                mesh.AddRouteRequestHandler<ExplicitRouteRequestHandler, TestRequest, TestReply>("route-request")
                    .AddRouteRequestHandler<AlternateRouteRequestHandler, TestRequest, TestReply>("route-request");
            }));

        Assert.Contains("Duplicate routed request handler 'profile:route-request'", exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public void PublicClients_ExposeOnlyUnifiedRouteClientForChannelAndNodeCalls()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(_ => { });

        using var provider = services.BuildServiceProvider();
        Assert.NotNull(provider.GetRequiredService<IZLinkRouteClient>());
    }

    private sealed record TestMessage(string Value);
    private sealed record TestRequest(string Value);
    private sealed record TestReply(string Value);

    private sealed class TestSendHandler : IZLinkSendHandler<TestMessage>
    {
        public ValueTask HandleAsync(
            TestMessage message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    [ZLinkHandlerGroup("mesh-handler-exposure-test")]
    private sealed class TestRequestHandler : IZLinkRequestHandler<TestRequest, TestReply>
    {
        public ValueTask<TestReply> HandleAsync(
            TestRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(new TestReply(request.Value));
    }

    private sealed class AlternateTestRequestHandler : IZLinkRequestHandler<TestRequest, TestReply>
    {
        public ValueTask<TestReply> HandleAsync(
            TestRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(new TestReply(request.Value));
    }

    private sealed class TestRouteSendHandler : IZLinkRouteSendHandler<TestMessage>
    {
        public ValueTask HandleAsync(
            TestMessage message,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class ExplicitRouteRequestHandler : IZLinkRouteRequestHandler<TestRequest, TestReply>
    {
        public ValueTask<TestReply> HandleAsync(
            TestRequest request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(new TestReply(request.Value));
    }

    private sealed class AlternateRouteRequestHandler : IZLinkRouteRequestHandler<TestRequest, TestReply>
    {
        public ValueTask<TestReply> HandleAsync(
            TestRequest request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(new TestReply(request.Value));
    }
}
